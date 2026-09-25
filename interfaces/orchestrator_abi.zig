//! Typed wire layouts for the planned Root control plane and fixed worker pool.
//!
//! These records describe bytes shared by different protection domains. Explicit
//! reserved members make every offset visible; no field may depend on an ABI's
//! implicit structure padding. The system ABI separately owns where the pages
//! are mapped and which child or channel owns them. No runtime code consumes
//! these definitions until the generated C and Erlang contracts are checked.

const std = @import("std");

pub const abi_version: u32 = 1;
pub const child_count: usize = 62;
pub const event_count: usize = 128;
pub const journal_capacity: usize = 15;
pub const journal_payload_size: usize = 192;

fn wireMagic(comptime ascii: []const u8) u64 {
    if (ascii.len != 8) @compileError("wire magic must have exactly eight bytes");
    var value: u64 = 0;
    for (ascii, 0..) |byte, offset| {
        value |= @as(u64, byte) << @as(u6, @intCast(offset * 8));
    }
    return value;
}

pub const magic = struct {
    pub const status = wireMagic("CHRYSTA1");
    pub const spec = wireMagic("CHRYSPE1");
    pub const command = wireMagic("CHRYCMD1");
    pub const reply = wireMagic("CHRYREP1");
    pub const worker_identity = wireMagic("CHRYWID1");
    pub const worker_status = wireMagic("CHRYWST1");
    pub const request = wireMagic("CHRYREQ1");
    pub const completion = wireMagic("CHRYCMP1");
};

pub const RootChildWireState = enum(u8) {
    unset = 0,
    live = 1,
    stopped = 2,
    gone = 3,
    quarantined = 4,
};

pub const RootDesired = enum(u8) {
    unset = 0,
    running = 1,
    stopped = 2,
};

pub const RootEventKind = enum(u8) {
    unset = 0,
    boot = 1,
    fault = 2,
    restart = 3,
    stop = 4,
    giveup = 5,
    quarantine = 6,
    reclaim = 7,
    control = 8,
    spec_reject = 9,
};

pub const PpOpcode = enum(u16) {
    unset = 0,
    hello = 1,
    restart_child = 2,
    start_slot = 3,
    stop_child = 4,
    resume_child = 5,
    reclaim_slot = 6,
    sync = 7,
};

pub const CtlResult = enum(u16) {
    unset = 0,
    ok = 1,
    refused = 2,
    ignored = 3,
    rate_limited = 4,
    bad_arg = 5,
    bad_version = 6,
    internal_error = 7,
};

// This is a decoder result, never a byte in a wire record.
pub const AbiReject = enum(u8) {
    ok = 0,
    magic = 1,
    version = 2,
    size = 3,
    checksum = 4,
    reserved = 5,
    unknown_kind = 6,
    range = 7,
    torn = 8,
};

pub const WorkerHealth = enum(u32) {
    unknown = 0,
    healthy = 1,
    unhealthy = 2,
};

pub const RequestKind = enum(u32) {
    work = 1,
    drain = 2,
    cancel = 3,
};

pub const CompletionKind = enum(u32) {
    work = 0x8001,
    drained = 0x8002,
    rejected = 0x8003,
};

pub const CompletionStatus = enum(u32) {
    ok = 0,
    bad_request = 1,
    stale_generation = 2,
    unsupported = 3,
    deadline_expired = 4,
    draining = 5,
    internal_error = 6,
};

pub const SlotPhase = enum(u32) {
    unset = 0,
    unassigned = 1,
    staging = 2,
    starting = 3,
    ready = 4,
    draining = 5,
    stopping = 6,
    backoff = 7,
    quarantined = 8,
    gone = 9,
};

pub const SlotEvent = enum(u8) {
    prepare = 1,
    assign = 2,
    identity_committed = 3,
    matching_readiness = 4,
    scale_down = 5,
    drain_complete = 6,
    root_stopped = 7,
    timeout = 8,
    fault = 9,
    retry = 10,
    window_exhausted = 11,
    reclaim = 12,
    lifetime_exhausted = 13,
};

pub const Transition = struct {
    from: SlotPhase,
    to: SlotPhase,
    event: SlotEvent,
};

pub const slot_transitions = [_]Transition{
    .{ .from = .unassigned, .to = .staging, .event = .prepare },
    .{ .from = .unassigned, .to = .starting, .event = .assign },
    .{ .from = .staging, .to = .starting, .event = .identity_committed },
    .{ .from = .starting, .to = .ready, .event = .matching_readiness },
    .{ .from = .ready, .to = .draining, .event = .scale_down },
    .{ .from = .draining, .to = .stopping, .event = .drain_complete },
    .{ .from = .stopping, .to = .unassigned, .event = .root_stopped },
    .{ .from = .starting, .to = .backoff, .event = .timeout },
    .{ .from = .starting, .to = .backoff, .event = .fault },
    .{ .from = .ready, .to = .backoff, .event = .fault },
    .{ .from = .draining, .to = .backoff, .event = .timeout },
    .{ .from = .draining, .to = .backoff, .event = .fault },
    .{ .from = .backoff, .to = .starting, .event = .retry },
    .{ .from = .backoff, .to = .quarantined, .event = .window_exhausted },
    .{ .from = .quarantined, .to = .unassigned, .event = .reclaim },
    .{ .from = .backoff, .to = .gone, .event = .lifetime_exhausted },
    .{ .from = .quarantined, .to = .gone, .event = .lifetime_exhausted },
};

pub const Diagnostic = struct {
    path: []const u8 = "",
    invariant: []const u8 = "",
};

pub const RawTransition = struct {
    from: u32,
    to: u32,
    event: u8,
};

pub const raw_transitions = blk: {
    var rows: [slot_transitions.len]RawTransition = undefined;
    for (slot_transitions, 0..) |row, i| {
        rows[i] = .{
            .from = @intFromEnum(row.from),
            .to = @intFromEnum(row.to),
            .event = @intFromEnum(row.event),
        };
    }
    break :blk rows;
};

fn enumContains(comptime T: type, value: anytype) bool {
    inline for (@typeInfo(T).@"enum".fields) |field| {
        if (field.value == value) return true;
    }
    return false;
}

pub fn validateTransitions(rows: []const RawTransition, diagnostic: *Diagnostic) !void {
    diagnostic.* = .{};
    for (rows, 0..) |row, i| {
        if (!enumContains(SlotPhase, row.from) or !enumContains(SlotPhase, row.to) or
            row.from == @intFromEnum(SlotPhase.unset) or row.to == @intFromEnum(SlotPhase.unset))
        {
            diagnostic.* = .{ .path = "slot_transitions", .invariant = "declared nonzero phase" };
            return error.InvalidTransitionPhase;
        }
        if (!enumContains(SlotEvent, row.event)) {
            diagnostic.* = .{ .path = "slot_transitions", .invariant = "declared event" };
            return error.InvalidTransitionEvent;
        }
        for (rows[0..i]) |previous| {
            if (row.from == previous.from and row.event == previous.event) {
                diagnostic.* = .{ .path = "slot_transitions", .invariant = "unique from/event" };
                return error.DuplicateTransition;
            }
        }
    }
}

pub const AtomicField = struct {
    record: []const u8,
    field: []const u8,
    width: u8,
};

// The C projection replaces these integer fields with lock-free _Atomic fields.
// The wire widths and offsets remain those of the extern structs below.
pub const atomic_fields = [_]AtomicField{
    .{ .record = "RootStatusHeader", .field = "seq", .width = 8 },
    .{ .record = "SpecHeader", .field = "active_bank", .width = 4 },
    .{ .record = "SpecBank", .field = "bank_seq", .width = 4 },
    .{ .record = "WorkerIdentityHeader", .field = "completion_ack", .width = 8 },
    .{ .record = "WorkerStatusHeader", .field = "request_ack", .width = 8 },
    .{ .record = "WorkerStatusHeader", .field = "status_seq", .width = 8 },
    .{ .record = "JournalHeader", .field = "published_seq", .width = 8 },
};

pub const RootStatusHeader = extern struct {
    magic: u64,
    abi_version: u32,
    child_count: u32,
    seq: u64,
    root_generation: u64,
    beam_incarnation: u64,
    now_ticks: u64,
    cntfrq: u64,
    event_head: u64,
    event_dropped: u64,
};

pub const RootChildStatus = extern struct {
    state: u8,
    desired: u8,
    flags: u16,
    lifetime_count: u32,
    effective_budget: u32,
    fault_label: u32,
    fault_pc: u64,
    fault_addr: u64,
    last_fault_ticks: u64,
    last_restart_ticks: u64,
    boot_ticks: u64,
    cumulative_down_ticks: u64,
    slot_generation: u64,
    window_count: u32,
    reserved: u32 = 0,
};

pub const RootEvent = extern struct {
    ticks: u64,
    kind: u8,
    child: u8,
    pad: u16 = 0,
    detail: u32,
    a: u64,
    b: u64,
};

pub const RootStatusPage = extern struct {
    header: RootStatusHeader,
    children: [child_count]RootChildStatus,
    events: [event_count]RootEvent,
    reserved_tail: [7256]u8 = [_]u8{0} ** 7256,
};

pub const SpecHeader = extern struct {
    magic: u64,
    abi_version: u32,
    active_bank: u32,
    reserved: [48]u8 = [_]u8{0} ** 48,
};

pub const SpecBank = extern struct {
    generation: u64,
    length: u32,
    crc32: u32,
    record_count: u32,
    bank_seq: u32,
    budget: [child_count]u32,
    desired: [child_count]u8,
    reserved_tail: [1682]u8 = [_]u8{0} ** 1682,
};

pub const SpecPage = extern struct {
    header: SpecHeader,
    banks: [2]SpecBank,
};

pub const CtlCommand = extern struct {
    magic: u64,
    version: u16,
    opcode: u16,
    reserved0: u32 = 0,
    args: [4]u64,
    reserved: [16]u8 = [_]u8{0} ** 16,
};

pub const CtlReply = extern struct {
    magic: u64,
    version: u16,
    result: u16,
    reserved0: u32 = 0,
    values: [4]u64,
    reserved: [16]u8 = [_]u8{0} ** 16,
};

pub const WorkerIdentityHeader = extern struct {
    magic: u64,
    abi_version: u32,
    slot: u16,
    class: u16,
    generation: u64,
    workload_ref: u64,
    desired_phase: u32,
    reserved0: u32 = 0,
    completion_ack: u64,
    reserved: [16]u8 = [_]u8{0} ** 16,
};

pub const WorkerIdentity = extern struct {
    header: WorkerIdentityHeader,
    reserved_tail: [4032]u8 = [_]u8{0} ** 4032,
};

pub const WorkerStatusHeader = extern struct {
    magic: u64,
    abi_version: u32,
    slot: u16,
    class: u16,
    generation: u64,
    phase: u32,
    health: u32,
    request_ack: u64,
    heartbeat_ticks: u64,
    accepted_count: u64,
    completed_count: u64,
    queue_depth: u32,
    reserved0: u32 = 0,
    failed_count: u64,
    status_seq: u64,
};

pub const WorkerStatus = extern struct {
    header: WorkerStatusHeader,
    reserved_tail: [4008]u8 = [_]u8{0} ** 4008,
};

pub const JournalHeader = extern struct {
    magic: u64,
    abi_version: u32,
    slot: u16,
    reserved0: u16 = 0,
    generation: u64,
    published_seq: u64,
    capacity: u32,
    entry_size: u32,
    reserved: [216]u8 = [_]u8{0} ** 216,
};

pub const JournalEntry = extern struct {
    generation: u64,
    sequence: u64,
    request_id: u64,
    workload_id: u64,
    deadline_ticks: u64,
    kind: u32,
    status: u32,
    payload_length: u16,
    flags: u16,
    reserved0: u32 = 0,
    payload: [journal_payload_size]u8,
    reserved1: [8]u8 = [_]u8{0} ** 8,
};

pub const JournalPage = extern struct {
    header: JournalHeader,
    entries: [journal_capacity]JournalEntry,
};

fn assertNoImplicitPadding(comptime T: type) void {
    const info = @typeInfo(T).@"struct";
    var next_offset: usize = 0;
    inline for (info.fields) |field| {
        if (@offsetOf(T, field.name) != next_offset)
            @compileError(@typeName(T) ++ " has implicit padding before " ++ field.name);
        next_offset += @sizeOf(field.type);
    }
    if (@sizeOf(T) != next_offset)
        @compileError(@typeName(T) ++ " has implicit tail padding");
}

comptime {
    for (.{
        RootStatusHeader, RootChildStatus,      RootEvent,      RootStatusPage,
        SpecHeader,       SpecBank,             SpecPage,       CtlCommand,
        CtlReply,         WorkerIdentityHeader, WorkerIdentity, WorkerStatusHeader,
        WorkerStatus,     JournalHeader,        JournalEntry,   JournalPage,
    }) |T| assertNoImplicitPadding(T);
}

pub fn validate(diagnostic: *Diagnostic) !void {
    diagnostic.* = .{};
    if (@sizeOf(RootStatusPage) != 16384 or @offsetOf(RootStatusPage, "events") != 5032) {
        diagnostic.* = .{ .path = "RootStatusPage", .invariant = "size and event offset" };
        return error.InvalidWireLayout;
    }
    if (@sizeOf(SpecPage) != 4096 or @sizeOf(SpecBank) != 2016 or
        @offsetOf(SpecBank, "bank_seq") != 20)
    {
        diagnostic.* = .{ .path = "SpecPage", .invariant = "bank size and sequence offset" };
        return error.InvalidWireLayout;
    }
    if (@sizeOf(WorkerIdentity) != 4096 or @sizeOf(WorkerStatus) != 4096 or
        @sizeOf(JournalPage) != 4096 or @sizeOf(JournalEntry) != 256)
    {
        diagnostic.* = .{ .path = "worker pages", .invariant = "page and entry sizes" };
        return error.InvalidWireLayout;
    }
    if (@offsetOf(RootStatusHeader, "seq") % @alignOf(u64) != 0 or
        @offsetOf(SpecHeader, "active_bank") % @alignOf(u32) != 0 or
        @offsetOf(SpecBank, "bank_seq") % @alignOf(u32) != 0 or
        @offsetOf(WorkerIdentityHeader, "completion_ack") % @alignOf(u64) != 0 or
        @offsetOf(WorkerStatusHeader, "request_ack") % @alignOf(u64) != 0 or
        @offsetOf(WorkerStatusHeader, "status_seq") % @alignOf(u64) != 0 or
        @offsetOf(JournalHeader, "published_seq") % @alignOf(u64) != 0)
    {
        diagnostic.* = .{ .path = "atomic_fields", .invariant = "natural alignment" };
        return error.InvalidAtomicAlignment;
    }
    try validateTransitions(&raw_transitions, diagnostic);
}

test "wire records have pinned sizes and offsets" {
    try std.testing.expectEqual(@as(usize, 72), @sizeOf(RootStatusHeader));
    try std.testing.expectEqual(@as(usize, 80), @sizeOf(RootChildStatus));
    try std.testing.expectEqual(@as(usize, 32), @sizeOf(RootEvent));
    try std.testing.expectEqual(@as(usize, 16384), @sizeOf(RootStatusPage));
    try std.testing.expectEqual(@as(usize, 5032), @offsetOf(RootStatusPage, "events"));
    try std.testing.expectEqual(@as(usize, 16), @offsetOf(RootStatusHeader, "seq"));
    try std.testing.expectEqual(@as(usize, 64), @sizeOf(SpecHeader));
    try std.testing.expectEqual(@as(usize, 2016), @sizeOf(SpecBank));
    try std.testing.expectEqual(@as(usize, 4096), @sizeOf(SpecPage));
    try std.testing.expectEqual(@as(usize, 64), @offsetOf(SpecPage, "banks"));
    try std.testing.expectEqual(@as(usize, 2080), @offsetOf(SpecPage, "banks") + @sizeOf(SpecBank));
    try std.testing.expectEqual(@as(usize, 20), @offsetOf(SpecBank, "bank_seq"));
    try std.testing.expectEqual(@as(usize, 334), @offsetOf(SpecBank, "reserved_tail"));
    try std.testing.expectEqual(@as(usize, 64), @sizeOf(CtlCommand));
    try std.testing.expectEqual(@as(usize, 64), @sizeOf(CtlReply));
    try std.testing.expectEqual(@as(usize, 64), @sizeOf(WorkerIdentityHeader));
    try std.testing.expectEqual(@as(usize, 4096), @sizeOf(WorkerIdentity));
    try std.testing.expectEqual(@as(usize, 88), @sizeOf(WorkerStatusHeader));
    try std.testing.expectEqual(@as(usize, 80), @offsetOf(WorkerStatusHeader, "status_seq"));
    try std.testing.expectEqual(@as(usize, 4096), @sizeOf(WorkerStatus));
    try std.testing.expectEqual(@as(usize, 256), @sizeOf(JournalHeader));
    try std.testing.expectEqual(@as(usize, 256), @sizeOf(JournalEntry));
    try std.testing.expectEqual(@as(usize, 4096), @sizeOf(JournalPage));
}

test "magic integer represents its declared raw bytes" {
    try std.testing.expectEqual(@as(u64, 0x3141545359524843), magic.status);
    inline for ("CHRYSTA1", 0..) |byte, offset| {
        try std.testing.expectEqual(byte, @as(u8, @truncate(magic.status >> @as(u6, @intCast(offset * 8)))));
    }
}

test "journal enum values match the protocol sketch" {
    try std.testing.expectEqual(@as(u32, 1), @intFromEnum(RequestKind.work));
    try std.testing.expectEqual(@as(u32, 3), @intFromEnum(RequestKind.cancel));
    try std.testing.expectEqual(@as(u32, 0x8001), @intFromEnum(CompletionKind.work));
    try std.testing.expectEqual(@as(u32, 0x8003), @intFromEnum(CompletionKind.rejected));
    try std.testing.expectEqual(@as(u32, 0), @intFromEnum(CompletionStatus.ok));
    try std.testing.expectEqual(@as(u32, 6), @intFromEnum(CompletionStatus.internal_error));
    try std.testing.expectEqual(@as(usize, 4), @sizeOf(WorkerHealth));
    try std.testing.expectEqual(@as(usize, 4), @sizeOf(SlotPhase));
}

test "typed contract and transition table are valid" {
    var diagnostic: Diagnostic = .{};
    try validate(&diagnostic);
    try std.testing.expectEqualStrings("", diagnostic.path);
}

test "transition validation rejects undeclared and duplicate edges" {
    var diagnostic: Diagnostic = .{};
    var bad = raw_transitions;
    bad[0].to = 99;
    try std.testing.expectError(error.InvalidTransitionPhase, validateTransitions(&bad, &diagnostic));
    try std.testing.expectEqualStrings("declared nonzero phase", diagnostic.invariant);

    bad = raw_transitions;
    bad[1].event = bad[0].event;
    try std.testing.expectError(error.DuplicateTransition, validateTransitions(&bad, &diagnostic));
    try std.testing.expectEqualStrings("unique from/event", diagnostic.invariant);
}

test "reserved fields default to zero" {
    const command: CtlCommand = .{
        .magic = magic.command,
        .version = abi_version,
        .opcode = @intFromEnum(PpOpcode.hello),
        .args = .{ 0, 0, 0, 0 },
    };
    try std.testing.expectEqual(@as(u32, 0), command.reserved0);
    for (command.reserved) |byte| try std.testing.expectEqual(@as(u8, 0), byte);

    const event: RootEvent = .{ .ticks = 0, .kind = 0, .child = 0, .detail = 0, .a = 0, .b = 0 };
    try std.testing.expectEqual(@as(u16, 0), event.pad);

    const status = std.mem.zeroes(RootStatusPage);
    const spec = std.mem.zeroes(SpecPage);
    const journal = std.mem.zeroes(JournalPage);
    for (status.reserved_tail) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
    for (spec.header.reserved) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
    for (spec.banks[0].reserved_tail) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
    for (journal.header.reserved) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
}
