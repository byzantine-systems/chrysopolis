//! Valid sample pages for tests and vectors; each passes its checker.
const std = @import("std");
const abi = @import("orchestrator_abi");

pub fn status() abi.RootStatusPage {
    var page = std.mem.zeroes(abi.RootStatusPage);
    page.header = .{ .magic = abi.magic.status, .abi_version = abi.abi_version, .child_count = abi.child_count, .seq = 2, .root_generation = 1, .beam_incarnation = 1, .now_ticks = 9, .cntfrq = 62_500_000, .event_head = 130, .event_dropped = 2, .applied_spec_generation = 1, .applied_bank = 0 };
    page.children[0].state = @intFromEnum(abi.RootChildWireState.live);
    page.children[0].desired = @intFromEnum(abi.RootDesired.running);
    for (&page.events) |*event| event.* = .{ .ticks = 1, .kind = @intFromEnum(abi.RootEventKind.boot), .child = abi.root_event_no_child, .detail = 0, .a = 0, .b = 0 };
    return page;
}

/// CRC over the bank with the CRC and sequence fields read as zero.
pub fn sealBank(bank: *abi.SpecBank) void {
    const seq = bank.bank_seq;
    bank.crc32 = 0;
    bank.bank_seq = 0;
    bank.crc32 = std.hash.Crc32.hash(std.mem.asBytes(bank));
    bank.bank_seq = seq;
}

/// Bank 0 is published and sealed; bank 1 is unpublished.
pub fn spec() abi.SpecPage {
    var page = std.mem.zeroes(abi.SpecPage);
    page.header.magic = abi.magic.spec;
    page.header.abi_version = abi.abi_version;
    page.banks[0] = .{ .generation = 1, .length = @sizeOf(abi.SpecBank), .crc32 = 0, .record_count = abi.child_count, .bank_seq = 2, .budget = @splat(3), .desired = @splat(@intFromEnum(abi.RootDesired.running)) };
    sealBank(&page.banks[0]);
    return page;
}

/// Both banks published and sealed; the header selects bank 1, the newer spec.
pub fn specTwoBanks() abi.SpecPage {
    var page = spec();
    page.header.active_bank = 1;
    page.banks[1] = page.banks[0];
    page.banks[1].generation = 2;
    page.banks[1].bank_seq = 4;
    sealBank(&page.banks[1]);
    return page;
}

pub fn ctlCommand() abi.CtlCommand {
    return .{ .magic = abi.magic.command, .version = abi.abi_version, .opcode = @intFromEnum(abi.PpOpcode.hello), .args = .{ 1, 2, 3, 4 } };
}

pub fn ctlReply() abi.CtlReply {
    return .{ .magic = abi.magic.reply, .version = abi.abi_version, .result = @intFromEnum(abi.CtlResult.ok), .values = .{ 5, 6, 7, 8 } };
}

pub fn workerIdentity() abi.WorkerIdentity {
    var page = std.mem.zeroes(abi.WorkerIdentity);
    page.header = .{ .magic = abi.magic.worker_identity, .abi_version = abi.abi_version, .slot = 3, .class = 0, .generation = 7, .workload_ref = 11, .desired_phase = @intFromEnum(abi.SlotPhase.ready), .completion_ack = 4 };
    return page;
}

pub fn workerStatus() abi.WorkerStatus {
    var page = std.mem.zeroes(abi.WorkerStatus);
    page.header = .{ .magic = abi.magic.worker_status, .abi_version = abi.abi_version, .slot = 3, .class = 0, .generation = 7, .phase = @intFromEnum(abi.SlotPhase.ready), .health = @intFromEnum(abi.WorkerHealth.healthy), .request_ack = 4, .heartbeat_ticks = 100, .accepted_count = 4, .completed_count = 3, .queue_depth = 1, .failed_count = 0, .status_seq = 6 };
    return page;
}

/// Sequences 6..20 are live, so the ring has wrapped.
pub fn journal(comptime completion: bool) abi.JournalPage {
    var page = std.mem.zeroes(abi.JournalPage);
    page.header = .{ .magic = if (completion) abi.magic.completion else abi.magic.request, .abi_version = abi.abi_version, .slot = 3, .generation = 7, .published_seq = 20, .capacity = abi.journal_capacity, .entry_size = @sizeOf(abi.JournalEntry) };
    for (6..21) |s| {
        const entry = &page.entries[(s - 1) % abi.journal_capacity];
        entry.generation = 7;
        entry.sequence = s;
        entry.kind = if (completion) @intFromEnum(abi.CompletionKind.work) else @intFromEnum(abi.RequestKind.work);
        // Payloads fill exactly their length; the newest uses the full 192 bytes.
        const length: u16 = if (s == 20) abi.journal_payload_size else @intCast(s * 13 % abi.journal_payload_size);
        entry.payload_length = length;
        @memset(entry.payload[0..length], @intCast(s));
    }
    return page;
}

/// A full request journal whose newest sequence is the u64 maximum.
pub fn journalAtMax() abi.JournalPage {
    var page = std.mem.zeroes(abi.JournalPage);
    page.header = .{ .magic = abi.magic.request, .abi_version = abi.abi_version, .slot = 0, .generation = 1, .published_seq = std.math.maxInt(u64), .capacity = abi.journal_capacity, .entry_size = @sizeOf(abi.JournalEntry) };
    var s: u64 = std.math.maxInt(u64) - abi.journal_capacity + 1;
    while (true) : (s += 1) {
        const entry = &page.entries[@intCast((s - 1) % abi.journal_capacity)];
        entry.generation = 1;
        entry.sequence = s;
        entry.kind = @intFromEnum(abi.RequestKind.drain);
        if (s == std.math.maxInt(u64)) break;
    }
    return page;
}

comptime {
    // Samples are built from native extern structs, so the host must be little-endian.
    std.debug.assert(@import("builtin").cpu.arch.endian() == .little);
}

/// A valid image for the named checker. Caller owns the bytes.
pub fn image(allocator: std.mem.Allocator, checker: []const u8) error{ OutOfMemory, NoSample }![]u8 {
    const eql = std.mem.eql;
    if (eql(u8, checker, "root_status_page")) return allocator.dupe(u8, std.mem.asBytes(&status()));
    if (eql(u8, checker, "spec_header") or eql(u8, checker, "spec_bank")) return allocator.dupe(u8, std.mem.asBytes(&spec()));
    if (eql(u8, checker, "ctl_command")) return allocator.dupe(u8, std.mem.asBytes(&ctlCommand()));
    if (eql(u8, checker, "ctl_reply")) return allocator.dupe(u8, std.mem.asBytes(&ctlReply()));
    if (eql(u8, checker, "worker_identity")) return allocator.dupe(u8, std.mem.asBytes(&workerIdentity()));
    if (eql(u8, checker, "worker_status")) return allocator.dupe(u8, std.mem.asBytes(&workerStatus()));
    if (eql(u8, checker, "request_journal")) return allocator.dupe(u8, std.mem.asBytes(&journal(false)));
    if (eql(u8, checker, "completion_journal")) return allocator.dupe(u8, std.mem.asBytes(&journal(true)));
    return error.NoSample;
}
