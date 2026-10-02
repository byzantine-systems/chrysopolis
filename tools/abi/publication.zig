//! Reference writers for the publication protocols, as explicit state machines. A reader may
//! copy the page between any two steps. Root's status writer is modelled one store per step, in
//! the order root_status_publish makes them; the other protocols' `write` groups payload stores
//! the protocol leaves unordered. Golden vectors snapshot the page after every step, so torn
//! states come from the protocols themselves. These are test models, not the Root or BEAM
//! writers; tests/host's root_control suite fuzzes the real writer and reader at byte level.
const std = @import("std");
const abi = @import("orchestrator_abi");
const checks = @import("checks");

const samples = checks.samples;
const Class = checks.Class;

fn at(comptime T: type, comptime path: []const u8) usize {
    return comptime checks.at(T, path);
}

pub const Protocol = enum {
    /// Root rewrites its status page under `header.seq`.
    status,
    /// A worker rewrites its status page under `header.status_seq`.
    worker_status,
    /// BEAM rewrites the inactive spec bank (bank 1), then selects it.
    spec_bank,
    /// A producer appends one journal entry, then advances `published_seq`.
    journal,
};

pub const Step = enum {
    /// The sequence becomes odd: the record is being written.
    open,
    /// Payload fields change.
    write,
    /// Status: the affected child row's state.
    row_state,
    /// Status: the affected child row's lifetime count.
    row_count,
    /// Status: the cause event at `event_head`.
    cause,
    /// Status: the outcome event at `event_head + 1`.
    outcome,
    /// Status: `event_head` moves past both events.
    head,
    /// Status: `event_dropped` follows the new head.
    dropped,
    /// Status: `now_ticks` stamps the publication.
    stamp,
    /// The CRC covers the new contents.
    seal,
    /// The sequence becomes the next even value: the record is published.
    close,
    /// `active_bank` selects the rewritten bank.
    flip,
    /// The entry for `published_seq + 1` lands in its slot.
    append,
    /// `published_seq` advances over the new entry.
    advance,
};

pub const Error = error{ StepNotInProtocol, Finished, Exhausted };

pub const Start = enum { sample, empty_journal };

pub const Trace = struct {
    name: []const u8,
    protocol: Protocol,
    checker: []const u8,
    /// Spec bank the reader checks; ignored by checkers without a selector.
    bank: usize = 0,
    start: Start = .sample,
    steps: []const Step,
    /// The reader's verdict on the starting page, then after each step.
    expect: []const Class,
};

const seqlock: []const Step = &.{ .open, .write, .close };
const status_publish: []const Step = &.{ .open, .row_state, .row_count, .cause, .outcome, .head, .dropped, .stamp, .close };
const bank_rewrite: []const Step = &.{ .open, .write, .seal, .close, .flip };
const append: []const Step = &.{ .append, .advance };

pub const traces = [_]Trace{
    // Every store between the odd and even sequence is torn, whatever it changed.
    .{ .name = "status", .protocol = .status, .checker = "root_status_page", .steps = status_publish, .expect = &.{ .ok, .torn, .torn, .torn, .torn, .torn, .torn, .torn, .torn, .ok } },
    .{ .name = "worker_status", .protocol = .worker_status, .checker = "worker_status", .steps = seqlock, .expect = &.{ .ok, .torn, .torn, .ok } },
    // Sealed but still odd is torn: the sequence outranks the checksum.
    .{ .name = "spec_rewritten_bank", .protocol = .spec_bank, .checker = "spec_bank", .bank = 1, .steps = bank_rewrite, .expect = &.{ .torn, .torn, .torn, .torn, .ok, .ok } },
    .{ .name = "spec_other_bank", .protocol = .spec_bank, .checker = "spec_bank", .bank = 0, .steps = bank_rewrite, .expect = &.{ .ok, .ok, .ok, .ok, .ok, .ok } },
    .{ .name = "spec_header", .protocol = .spec_bank, .checker = "spec_header", .steps = bank_rewrite, .expect = &.{ .ok, .ok, .ok, .ok, .ok, .ok } },
    // A full ring overwrites its oldest live slot before the head moves past it.
    .{ .name = "journal_full", .protocol = .journal, .checker = "request_journal", .steps = append, .expect = &.{ .ok, .torn, .ok } },
    // Below capacity the new slot is outside the window until the head advances.
    .{ .name = "journal_empty", .protocol = .journal, .checker = "request_journal", .start = .empty_journal, .steps = append ++ append, .expect = &.{ .ok, .ok, .ok, .ok, .ok } },
};

comptime {
    for (traces) |trace| {
        if (trace.expect.len != trace.steps.len + 1) @compileError(trace.name ++ ": one verdict per snapshot");
    }
}

fn load(comptime T: type, page: []const u8, offset: usize) T {
    return std.mem.readInt(T, page[offset..][0..@sizeOf(T)], .little);
}

fn store(comptime T: type, page: []u8, offset: usize, value: T) void {
    std.mem.writeInt(T, page[offset..][0..@sizeOf(T)], value, .little);
}

fn bump(comptime T: type, page: []u8, offset: usize) void {
    store(T, page, offset, load(T, page, offset) +% 1);
}

/// Applies one writer store to `page`, a whole record image.
pub fn apply(protocol: Protocol, page: []u8, step: Step) Error!void {
    switch (protocol) {
        .status => {
            const P = abi.RootStatusPage;
            const row = at(P, "children") + @sizeOf(abi.RootChildStatus);
            switch (step) {
                .open, .close => {
                    const seq = load(u64, page, at(P, "header.seq"));
                    if (seq == std.math.maxInt(u64)) return error.Exhausted;
                    store(u64, page, at(P, "header.seq"), seq + 1);
                },
                // One callback: child 1 faults and is restarted. The row and
                // both events land before the head moves, as in the C writer.
                .row_state => store(u8, page, row + at(abi.RootChildStatus, "state"), @intFromEnum(abi.RootChildWireState.live)),
                .row_count => store(u32, page, row + at(abi.RootChildStatus, "lifetime_count"), 1),
                .cause, .outcome => {
                    const head = load(u64, page, at(P, "header.event_head"));
                    const event: abi.RootEvent = if (step == .cause)
                        .{ .ticks = 9, .kind = @intFromEnum(abi.RootEventKind.fault), .child = 1, .detail = 6, .a = 0, .b = 0 }
                    else
                        .{ .ticks = 10, .kind = @intFromEnum(abi.RootEventKind.restart), .child = 1, .detail = 0, .a = 1, .b = 0 };
                    const slot: usize = @intCast((head +% @intFromBool(step == .outcome)) % abi.event_count);
                    @memcpy(page[at(P, "events") + slot * @sizeOf(abi.RootEvent) ..][0..@sizeOf(abi.RootEvent)], std.mem.asBytes(&event));
                },
                .head => {
                    const head = load(u64, page, at(P, "header.event_head"));
                    if (head > std.math.maxInt(u64) - 2) return error.Exhausted;
                    store(u64, page, at(P, "header.event_head"), head + 2);
                },
                .dropped => {
                    const head = load(u64, page, at(P, "header.event_head"));
                    store(u64, page, at(P, "header.event_dropped"), if (head > abi.event_count) head - abi.event_count else 0);
                },
                .stamp => store(u64, page, at(P, "header.now_ticks"), 10),
                .write, .seal, .flip, .append, .advance => return error.StepNotInProtocol,
            }
        },
        .worker_status => {
            const P = abi.WorkerStatus;
            switch (step) {
                .open, .close => bump(u64, page, at(P, "header.status_seq")),
                .write => {
                    bump(u64, page, at(P, "header.heartbeat_ticks"));
                    bump(u64, page, at(P, "header.completed_count"));
                },
                .seal, .flip, .append, .advance, .row_state, .row_count, .cause, .outcome, .head, .dropped, .stamp => return error.StepNotInProtocol,
            }
        },
        .spec_bank => {
            const P = abi.SpecPage;
            const bank = at(P, "banks") + @sizeOf(abi.SpecBank);
            switch (step) {
                .open, .close => bump(u32, page, bank + at(abi.SpecBank, "bank_seq")),
                .write => {
                    store(u64, page, bank + at(abi.SpecBank, "generation"), 2);
                    store(u32, page, bank + at(abi.SpecBank, "length"), @sizeOf(abi.SpecBank));
                    store(u32, page, bank + at(abi.SpecBank, "record_count"), abi.child_count);
                    for (0..abi.child_count) |child| {
                        store(u32, page, bank + at(abi.SpecBank, "budget") + child * 4, 5);
                        page[bank + at(abi.SpecBank, "desired") + child] = @intFromEnum(abi.RootDesired.running);
                    }
                },
                .seal => {
                    // The CRC reads the sequence as zero, so the writer may seal while it is odd.
                    var value = std.mem.bytesToValue(abi.SpecBank, page[bank..][0..@sizeOf(abi.SpecBank)]);
                    samples.sealBank(&value);
                    @memcpy(page[bank..][0..@sizeOf(abi.SpecBank)], std.mem.asBytes(&value));
                },
                .flip => store(u32, page, at(P, "header.active_bank"), 1),
                .append, .advance, .row_state, .row_count, .cause, .outcome, .head, .dropped, .stamp => return error.StepNotInProtocol,
            }
        },
        .journal => {
            const P = abi.JournalPage;
            const E = abi.JournalEntry;
            const head = load(u64, page, at(P, "header.published_seq"));
            switch (step) {
                .append => {
                    const next = head +% 1;
                    const slot: usize = @intCast((next -% 1) % abi.journal_capacity);
                    var entry = std.mem.zeroes(E);
                    entry.generation = load(u64, page, at(P, "header.generation"));
                    entry.sequence = next;
                    entry.request_id = next;
                    entry.kind = @intFromEnum(abi.RequestKind.work);
                    entry.payload_length = 3;
                    entry.payload[0..3].* = .{ 1, 2, 3 };
                    @memcpy(page[at(P, "entries") + slot * @sizeOf(E) ..][0..@sizeOf(E)], std.mem.asBytes(&entry));
                },
                .advance => store(u64, page, at(P, "header.published_seq"), head +% 1),
                .open, .write, .seal, .close, .flip, .row_state, .row_count, .cause, .outcome, .head, .dropped, .stamp => return error.StepNotInProtocol,
            }
        },
    }
}

/// The starting page of a trace. Caller owns the bytes.
pub fn startPage(allocator: std.mem.Allocator, trace: Trace) error{ OutOfMemory, NoSample }![]u8 {
    return switch (trace.start) {
        .sample => samples.image(allocator, trace.checker),
        .empty_journal => blk: {
            var page = samples.journal(false);
            page.header.published_seq = 0;
            page.entries = std.mem.zeroes(@TypeOf(page.entries));
            break :blk allocator.dupe(u8, std.mem.asBytes(&page));
        },
    };
}

/// A writer walking one trace over a borrowed page: `step` makes the next store.
pub const Writer = struct {
    trace: Trace,
    page: []u8,
    next: usize = 0,

    pub fn init(trace: Trace, page: []u8) Writer {
        return .{ .trace = trace, .page = page };
    }

    /// Makes the next store and returns it; `Finished` once every step is applied.
    pub fn step(self: *Writer) Error!Step {
        if (self.next == self.trace.steps.len) return error.Finished;
        const current = self.trace.steps[self.next];
        try apply(self.trace.protocol, self.page, current);
        self.next += 1;
        return current;
    }

    /// The verdict a reader copying the page now must reach.
    pub fn expected(self: Writer) Class {
        return self.trace.expect[self.next];
    }
};

/// Bytes a step may change for a protocol; every other byte must survive it.
fn owned(protocol: Protocol) checks.Span {
    return switch (protocol) {
        .status => .{ .offset = 0, .len = @sizeOf(abi.RootStatusPage) - @sizeOf(@FieldType(abi.RootStatusPage, "reserved_tail")) },
        .worker_status => .{ .offset = 0, .len = @sizeOf(abi.WorkerStatusHeader) },
        .spec_bank => .{ .offset = at(abi.SpecPage, "banks") + @sizeOf(abi.SpecBank), .len = @sizeOf(abi.SpecBank) },
        .journal => .{ .offset = 0, .len = @sizeOf(abi.JournalPage) },
    };
}

test "every trace reaches its stated verdict at every snapshot" {
    const allocator = std.testing.allocator;
    for (traces) |trace| {
        const page = try startPage(allocator, trace);
        defer allocator.free(page);
        const checker = checks.findChecker(&checks.checkers, trace.checker).?;
        var writer: Writer = .init(trace, page);
        while (true) {
            const actual = checks.verdict(checker, page, trace.bank);
            if (actual != writer.expected()) {
                std.debug.print("{s} after {d} steps: states {t}, reference gives {t}\n", .{ trace.name, writer.next, writer.expected(), actual });
                return error.WrongVerdict;
            }
            _ = writer.step() catch |err| switch (err) {
                error.Finished => break,
                error.StepNotInProtocol => return err,
                error.Exhausted => return err,
            };
        }
    }
}

test "the rewritten bank seals to the value a fresh seal gives" {
    var page = samples.spec();
    for (bank_rewrite) |step| try apply(.spec_bank, std.mem.asBytes(&page), step);
    var resealed = page.banks[1];
    samples.sealBank(&resealed);
    try std.testing.expectEqual(resealed.crc32, page.banks[1].crc32);
    try std.testing.expectEqual(@as(u32, 2), page.banks[1].bank_seq);
    try std.testing.expectEqual(@as(u32, 1), page.header.active_bank);
}

test "steps outside a protocol are refused and change nothing" {
    const refused = [_]struct { Protocol, Step }{ .{ .status, .seal }, .{ .status, .write }, .{ .worker_status, .cause }, .{ .worker_status, .flip }, .{ .spec_bank, .append }, .{ .journal, .open } };
    for (refused) |case| {
        var page: [@sizeOf(abi.RootStatusPage)]u8 = @splat(0xa5);
        try std.testing.expectError(error.StepNotInProtocol, apply(case[0], &page, case[1]));
        for (page) |byte| try std.testing.expectEqual(@as(u8, 0xa5), byte);
    }
}

test "a finished writer stays finished" {
    var page = samples.ctlCommand();
    var writer: Writer = .init(.{ .name = "none", .protocol = .status, .checker = "ctl_command", .steps = &.{}, .expect = &.{.ok} }, std.mem.asBytes(&page));
    try std.testing.expectError(error.Finished, writer.step());
    try std.testing.expectError(error.Finished, writer.step());
}

test "a step touches only its protocol's bytes" {
    const allocator = std.testing.allocator;
    for (traces) |trace| {
        const page = try startPage(allocator, trace);
        defer allocator.free(page);
        const before = try allocator.dupe(u8, page);
        defer allocator.free(before);
        const region = owned(trace.protocol);
        var writer: Writer = .init(trace, page);
        while (writer.step()) |step| {
            // The flip is the one store outside the rewritten bank.
            if (step == .flip) continue;
            try std.testing.expectEqualSlices(u8, before[0..region.offset], page[0..region.offset]);
            try std.testing.expectEqualSlices(u8, before[region.offset + region.len ..], page[region.offset + region.len ..]);
        } else |err| try std.testing.expectEqual(error.Finished, err);
    }
}

test "each status step changes only the bytes of its one store" {
    var page = samples.status();
    const bytes = std.mem.asBytes(&page);
    const P = abi.RootStatusPage;
    const row = at(P, "children") + @sizeOf(abi.RootChildStatus);
    const head = page.header.event_head;
    const event_size = @sizeOf(abi.RootEvent);
    const stores = [_]struct { Step, usize, usize }{
        .{ .open, at(P, "header.seq"), 8 },
        .{ .row_state, row + at(abi.RootChildStatus, "state"), 1 },
        .{ .row_count, row + at(abi.RootChildStatus, "lifetime_count"), 4 },
        .{ .cause, at(P, "events") + @as(usize, @intCast(head % abi.event_count)) * event_size, event_size },
        .{ .outcome, at(P, "events") + @as(usize, @intCast((head + 1) % abi.event_count)) * event_size, event_size },
        .{ .head, at(P, "header.event_head"), 8 },
        .{ .dropped, at(P, "header.event_dropped"), 8 },
        .{ .stamp, at(P, "header.now_ticks"), 8 },
        .{ .close, at(P, "header.seq"), 8 },
    };
    comptime std.debug.assert(stores.len == status_publish.len);
    for (stores, status_publish) |expected, step| {
        try std.testing.expectEqual(expected[0], step);
        var before: [@sizeOf(P)]u8 = undefined;
        @memcpy(&before, bytes);
        try apply(.status, bytes, step);
        for (bytes, before, 0..) |now, was, offset| {
            if (now != was and (offset < expected[1] or offset >= expected[1] + expected[2])) {
                std.debug.print("{t} changed byte {d} outside its store\n", .{ step, offset });
                return error.StoreOutsideField;
            }
        }
    }
}
