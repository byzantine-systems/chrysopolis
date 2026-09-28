//! Ordered snapshot checks; every generated codec emits its checkers from these tables.
//! Verdict: `size`, then an invalid selector (`range`), then the first class in
//! `precedence` with any violation anywhere in the record.
const std = @import("std");
const abi = @import("orchestrator_abi");
const model = @import("model");

pub const Class = abi.AbiReject;

pub const precedence = [_]Class{ .size, .magic, .version, .torn, .checksum, .reserved, .unknown_kind, .range };

/// Position in `precedence`; null for `.ok`, which is never a rule class.
pub fn rank(class: Class) ?usize {
    return std.mem.indexOfScalar(Class, &precedence, class);
}

comptime {
    // Every rejection class has a rank; only `.ok` lacks one.
    for (std.enums.values(Class)) |class| {
        if ((std.mem.indexOfScalar(Class, &precedence, class) == null) != (class == .ok))
            @compileError("precedence must list every rejection class");
    }
}

pub const Error = error{
    UnknownCheckedRecord,
    InvalidCheckerSize,
    InvalidCheckScope,
    RuleOrder,
    RuleClass,
    UnknownRuleEnum,
    UnknownRuleMagic,
    InvalidRuleField,
    TooManyReservedLeaves,
    UncoveredReserved,
    MissingSeqlockRule,
    DuplicateChecker,
};

/// A byte span relative to the rule's scope.
pub const Span = struct {
    offset: usize,
    len: usize,
};

/// An unsigned little-endian integer relative to the rule's scope.
pub const Int = struct {
    offset: usize,
    width: usize,
};

pub const Rows = struct {
    base: usize,
    stride: usize,
    count: usize,
};

pub const WindowMode = enum {
    /// Indexes `[head - min(head, cap), head)`, slot `i % cap` (status events).
    ring,
    /// Sequences `(head - min(head, cap), head]`, slot `(s - 1) % cap` (journals).
    journal,
};

pub const Window = struct {
    /// Absolute offset of the u64 head.
    head_offset: usize,
    base: usize,
    stride: usize,
    capacity: usize,
    mode: WindowMode,
};

pub const Scope = union(enum) {
    whole,
    /// Every row of a fixed array.
    rows: Rows,
    /// Only published rows; the rest are not interpreted.
    window: Window,
    /// The row picked by the checker's selector argument.
    selected: Rows,
};

pub const Compare = struct {
    int: Int,
    value: u64,
};

pub const Member = struct {
    int: Int,
    enumeration: []const u8,
    /// Reject a declared zero tag (`unset` is never published here).
    reject_zero: bool,
};

pub const Crc = struct {
    span: Span,
    /// Stored CRC; read as zero while computing, like `zeroed`.
    crc: Int,
    zeroed: []const Span,
};

pub const EntryIdentity = struct {
    /// Absolute offset of the header generation.
    header_generation: usize,
    generation: Int,
    sequence: Int,
};

pub const PayloadTail = struct {
    length: Int,
    payload: Span,
};

pub const Check = union(enum) {
    magic: Compare,
    version: Compare,
    /// Zero (unpublished) or odd (in progress).
    seqlock: Int,
    /// Zero means unpublished.
    nonzero: Int,
    crc32: Crc,
    zero: Span,
    member: Member,
    equal: Compare,
    at_most: Compare,
    /// A live entry repeats the header generation and its own sequence.
    entry_identity: EntryIdentity,
    /// Payload bytes after `min(length, payload.len)` are zero.
    payload_tail: PayloadTail,
};

pub const Rule = struct {
    class: Class,
    scope: Scope,
    check: Check,
};

pub const Checker = struct {
    /// C `chryso_check_<name>`.
    name: []const u8,
    record: []const u8,
    size: usize,
    /// Set when the caller picks one row (the spec bank).
    selector: ?Rows = null,
    rules: []const Rule,
};

/// Offset of a dotted field path inside T, resolved at compile time.
pub fn at(comptime T: type, comptime path: []const u8) usize {
    comptime {
        var current = T;
        var offset: usize = 0;
        var parts = std.mem.splitScalar(u8, path, '.');
        while (parts.next()) |part| {
            if (!@hasField(current, part)) @compileError(@typeName(current) ++ " has no field " ++ part);
            offset += @offsetOf(current, part);
            current = @FieldType(current, part);
        }
        return offset;
    }
}

fn fieldType(comptime T: type, comptime path: []const u8) type {
    comptime {
        var current = T;
        var parts = std.mem.splitScalar(u8, path, '.');
        while (parts.next()) |part| current = @FieldType(current, part);
        return current;
    }
}

pub fn int(comptime T: type, comptime path: []const u8) Int {
    return .{ .offset = at(T, path), .width = @sizeOf(fieldType(T, path)) };
}

pub fn span(comptime T: type, comptime path: []const u8) Span {
    return .{ .offset = at(T, path), .len = @sizeOf(fieldType(T, path)) };
}

fn rowsOf(comptime T: type, comptime path: []const u8) Rows {
    const Array = @typeInfo(fieldType(T, path)).array;
    return .{ .base = at(T, path), .stride = @sizeOf(Array.child), .count = Array.len };
}

fn windowOf(comptime T: type, comptime path: []const u8, comptime head: []const u8, mode: WindowMode) Window {
    const rows = rowsOf(T, path);
    return .{ .head_offset = at(T, head), .base = rows.base, .stride = rows.stride, .capacity = rows.count, .mode = mode };
}

fn rule(class: Class, scope: Scope, check: Check) Rule {
    return .{ .class = class, .scope = scope, .check = check };
}

const Status = abi.RootStatusPage;
const Child = abi.RootChildStatus;
const Event = abi.RootEvent;
const Spec = abi.SpecPage;
const Bank = abi.SpecBank;
const Journal = abi.JournalPage;
const Entry = abi.JournalEntry;

const status_rows: Scope = .{ .rows = rowsOf(Status, "children") };
const status_events: Scope = .{ .window = windowOf(Status, "events", "header.event_head", .ring) };
const spec_bank: Scope = .{ .selected = rowsOf(Spec, "banks") };
const journal_entries: Scope = .{ .window = windowOf(Journal, "entries", "header.published_seq", .journal) };

const root_status_rules = [_]Rule{
    rule(.magic, .whole, .{ .magic = .{ .int = int(Status, "header.magic"), .value = abi.magic.status } }),
    rule(.version, .whole, .{ .version = .{ .int = int(Status, "header.abi_version"), .value = abi.abi_version } }),
    rule(.torn, .whole, .{ .seqlock = int(Status, "header.seq") }),
    rule(.reserved, .whole, .{ .zero = span(Status, "header.reserved0") }),
    rule(.reserved, status_rows, .{ .zero = span(Child, "reserved") }),
    rule(.reserved, status_events, .{ .zero = span(Event, "pad") }),
    rule(.reserved, .whole, .{ .zero = span(Status, "reserved_tail") }),
    rule(.unknown_kind, status_rows, .{ .member = .{ .int = int(Child, "state"), .enumeration = "RootChildWireState", .reject_zero = false } }),
    rule(.unknown_kind, status_rows, .{ .member = .{ .int = int(Child, "desired"), .enumeration = "RootDesired", .reject_zero = false } }),
    rule(.unknown_kind, status_events, .{ .member = .{ .int = int(Event, "kind"), .enumeration = "RootEventKind", .reject_zero = true } }),
    rule(.range, .whole, .{ .equal = .{ .int = int(Status, "header.child_count"), .value = abi.child_count } }),
    rule(.range, .whole, .{ .at_most = .{ .int = int(Status, "header.applied_bank"), .value = 1 } }),
    rule(.range, status_events, .{ .at_most = .{ .int = int(Event, "child"), .value = abi.child_count - 1 } }),
};

const spec_header_rules = [_]Rule{
    rule(.magic, .whole, .{ .magic = .{ .int = int(Spec, "header.magic"), .value = abi.magic.spec } }),
    rule(.version, .whole, .{ .version = .{ .int = int(Spec, "header.abi_version"), .value = abi.abi_version } }),
    rule(.reserved, .whole, .{ .zero = span(Spec, "header.reserved") }),
    rule(.range, .whole, .{ .at_most = .{ .int = int(Spec, "header.active_bank"), .value = 1 } }),
};

const bank_crc_zeroed = [_]Span{span(Bank, "bank_seq")};

const spec_bank_rules = [_]Rule{
    rule(.torn, spec_bank, .{ .seqlock = int(Bank, "bank_seq") }),
    rule(.checksum, spec_bank, .{ .crc32 = .{
        .span = .{ .offset = 0, .len = @sizeOf(Bank) },
        .crc = int(Bank, "crc32"),
        .zeroed = &bank_crc_zeroed,
    } }),
    rule(.reserved, spec_bank, .{ .zero = span(Bank, "reserved_tail") }),
    rule(.range, spec_bank, .{ .equal = .{ .int = int(Bank, "length"), .value = @sizeOf(Bank) } }),
    rule(.range, spec_bank, .{ .equal = .{ .int = int(Bank, "record_count"), .value = abi.child_count } }),
};

fn ctlRules(comptime T: type, comptime magic: u64, comptime kind: []const u8, comptime enumeration: []const u8) [5]Rule {
    return .{
        rule(.magic, .whole, .{ .magic = .{ .int = int(T, "magic"), .value = magic } }),
        rule(.version, .whole, .{ .version = .{ .int = int(T, "version"), .value = abi.abi_version } }),
        rule(.reserved, .whole, .{ .zero = span(T, "reserved0") }),
        rule(.reserved, .whole, .{ .zero = span(T, "reserved") }),
        rule(.unknown_kind, .whole, .{ .member = .{ .int = int(T, kind), .enumeration = enumeration, .reject_zero = true } }),
    };
}

const ctl_command_rules = ctlRules(abi.CtlCommand, abi.magic.command, "opcode", "PpOpcode");
const ctl_reply_rules = ctlRules(abi.CtlReply, abi.magic.reply, "result", "CtlResult");

const Identity = abi.WorkerIdentity;
const worker_identity_rules = [_]Rule{
    rule(.magic, .whole, .{ .magic = .{ .int = int(Identity, "header.magic"), .value = abi.magic.worker_identity } }),
    rule(.version, .whole, .{ .version = .{ .int = int(Identity, "header.abi_version"), .value = abi.abi_version } }),
    rule(.torn, .whole, .{ .nonzero = int(Identity, "header.generation") }),
    rule(.reserved, .whole, .{ .zero = span(Identity, "header.reserved0") }),
    rule(.reserved, .whole, .{ .zero = span(Identity, "header.reserved") }),
    rule(.reserved, .whole, .{ .zero = span(Identity, "reserved_tail") }),
    rule(.unknown_kind, .whole, .{ .member = .{ .int = int(Identity, "header.desired_phase"), .enumeration = "SlotPhase", .reject_zero = true } }),
};

const WorkerStatus = abi.WorkerStatus;
const worker_status_rules = [_]Rule{
    rule(.magic, .whole, .{ .magic = .{ .int = int(WorkerStatus, "header.magic"), .value = abi.magic.worker_status } }),
    rule(.version, .whole, .{ .version = .{ .int = int(WorkerStatus, "header.abi_version"), .value = abi.abi_version } }),
    rule(.torn, .whole, .{ .seqlock = int(WorkerStatus, "header.status_seq") }),
    rule(.reserved, .whole, .{ .zero = span(WorkerStatus, "header.reserved0") }),
    rule(.reserved, .whole, .{ .zero = span(WorkerStatus, "reserved_tail") }),
    rule(.unknown_kind, .whole, .{ .member = .{ .int = int(WorkerStatus, "header.phase"), .enumeration = "SlotPhase", .reject_zero = false } }),
    rule(.unknown_kind, .whole, .{ .member = .{ .int = int(WorkerStatus, "header.health"), .enumeration = "WorkerHealth", .reject_zero = false } }),
};

fn journalRules(comptime magic: u64, comptime completion: bool) [15]Rule {
    const head = [_]Rule{
        rule(.magic, .whole, .{ .magic = .{ .int = int(Journal, "header.magic"), .value = magic } }),
        rule(.version, .whole, .{ .version = .{ .int = int(Journal, "header.abi_version"), .value = abi.abi_version } }),
        rule(.torn, .whole, .{ .nonzero = int(Journal, "header.generation") }),
        rule(.torn, journal_entries, .{ .entry_identity = .{
            .header_generation = at(Journal, "header.generation"),
            .generation = int(Entry, "generation"),
            .sequence = int(Entry, "sequence"),
        } }),
        rule(.reserved, .whole, .{ .zero = span(Journal, "header.reserved0") }),
        rule(.reserved, .whole, .{ .zero = span(Journal, "header.reserved") }),
        rule(.reserved, journal_entries, .{ .zero = span(Entry, "flags") }),
        rule(.reserved, journal_entries, .{ .zero = span(Entry, "reserved0") }),
        rule(.reserved, journal_entries, .{ .zero = span(Entry, "reserved1") }),
    };
    // Requests carry no status.
    const request_status = [_]Rule{rule(.reserved, journal_entries, .{ .zero = span(Entry, "status") })};
    const tail = [_]Rule{
        rule(.reserved, journal_entries, .{ .payload_tail = .{ .length = int(Entry, "payload_length"), .payload = span(Entry, "payload") } }),
    };
    const kinds = if (completion) [_]Rule{
        rule(.unknown_kind, journal_entries, .{ .member = .{ .int = int(Entry, "kind"), .enumeration = "CompletionKind", .reject_zero = false } }),
        rule(.unknown_kind, journal_entries, .{ .member = .{ .int = int(Entry, "status"), .enumeration = "CompletionStatus", .reject_zero = false } }),
    } else [_]Rule{
        rule(.unknown_kind, journal_entries, .{ .member = .{ .int = int(Entry, "kind"), .enumeration = "RequestKind", .reject_zero = false } }),
    };
    const ranges = [_]Rule{
        rule(.range, .whole, .{ .equal = .{ .int = int(Journal, "header.capacity"), .value = abi.journal_capacity } }),
        rule(.range, .whole, .{ .equal = .{ .int = int(Journal, "header.entry_size"), .value = @sizeOf(Entry) } }),
        rule(.range, journal_entries, .{ .at_most = .{ .int = int(Entry, "payload_length"), .value = abi.journal_payload_size } }),
    };
    return head ++ (if (completion) [_]Rule{} else request_status) ++ tail ++ kinds ++ ranges;
}

const request_journal_rules = journalRules(abi.magic.request, false);
const completion_journal_rules = journalRules(abi.magic.completion, true);

pub const checkers = [_]Checker{
    .{ .name = "root_status_page", .record = "RootStatusPage", .size = @sizeOf(Status), .rules = &root_status_rules },
    .{ .name = "spec_header", .record = "SpecPage", .size = @sizeOf(Spec), .rules = &spec_header_rules },
    .{ .name = "spec_bank", .record = "SpecPage", .size = @sizeOf(Spec), .selector = rowsOf(Spec, "banks"), .rules = &spec_bank_rules },
    .{ .name = "ctl_command", .record = "CtlCommand", .size = @sizeOf(abi.CtlCommand), .rules = &ctl_command_rules },
    .{ .name = "ctl_reply", .record = "CtlReply", .size = @sizeOf(abi.CtlReply), .rules = &ctl_reply_rules },
    .{ .name = "worker_identity", .record = "WorkerIdentity", .size = @sizeOf(Identity), .rules = &worker_identity_rules },
    .{ .name = "worker_status", .record = "WorkerStatus", .size = @sizeOf(WorkerStatus), .rules = &worker_status_rules },
    .{ .name = "request_journal", .record = "JournalPage", .size = @sizeOf(Journal), .rules = &request_journal_rules },
    .{ .name = "completion_journal", .record = "JournalPage", .size = @sizeOf(Journal), .rules = &completion_journal_rules },
};

pub fn findChecker(list: []const Checker, name: []const u8) ?Checker {
    for (list) |checker| {
        if (std.mem.eql(u8, checker.name, name)) return checker;
    }
    return null;
}

pub fn findRecord(description: model.Description, name: []const u8) ?model.Record {
    for (description.records) |record| {
        if (std.mem.eql(u8, record.name, name)) return record;
    }
    return null;
}

pub fn findEnum(description: model.Description, name: []const u8) ?model.Enum {
    for (description.enums) |enumeration| {
        if (std.mem.eql(u8, enumeration.name, name)) return enumeration;
    }
    return null;
}

fn isReservedName(name: []const u8) bool {
    return std.mem.startsWith(u8, name, "reserved") or std.mem.startsWith(u8, name, "pad");
}

/// First-instance base and length of a rule's scope.
fn scopeExtent(checker: Checker, scope: Scope) struct { base: usize, len: usize } {
    return switch (scope) {
        .whole => .{ .base = 0, .len = checker.size },
        .rows, .selected => |rows| .{ .base = rows.base, .len = rows.stride },
        .window => |window| .{ .base = window.base, .len = window.stride },
    };
}

fn fits(offset: usize, len: usize, extent: usize) bool {
    const end = std.math.add(usize, offset, len) catch return false;
    return len != 0 and end <= extent;
}

fn scopeFits(checker: Checker, scope: Scope) bool {
    return switch (scope) {
        .whole => true,
        .rows, .selected => |rows| rows.stride != 0 and rows.count != 0 and
            fits(rows.base, std.math.mul(usize, rows.stride, rows.count) catch return false, checker.size),
        .window => |window| window.stride != 0 and window.capacity != 0 and
            fits(window.head_offset, 8, checker.size) and
            fits(window.base, std.math.mul(usize, window.stride, window.capacity) catch return false, checker.size),
    };
}

fn intFits(value: Int, extent: usize) bool {
    return (value.width == 1 or value.width == 2 or value.width == 4 or value.width == 8) and fits(value.offset, value.width, extent);
}

fn valueFits(value: u64, width: usize) bool {
    return width >= 8 or value < (@as(u64, 1) << @as(u6, @intCast(width * 8)));
}

fn reject(diagnostic: *abi.Diagnostic, path: []const u8, invariant: []const u8, err: Error) Error {
    diagnostic.* = .{ .path = path, .invariant = invariant };
    return err;
}

/// Geometry, rule order, enum references and value widths of one checker.
pub fn validateChecker(checker: Checker, description: model.Description, diagnostic: *abi.Diagnostic) Error!void {
    diagnostic.* = .{};
    const record = findRecord(description, checker.record) orelse
        return reject(diagnostic, checker.name, "checked record exists", error.UnknownCheckedRecord);
    if (record.size != checker.size)
        return reject(diagnostic, checker.name, "checker size is the record size", error.InvalidCheckerSize);
    if (checker.selector) |selector| {
        if (!scopeFits(checker, .{ .selected = selector }))
            return reject(diagnostic, checker.name, "selector rows fit the record", error.InvalidCheckScope);
    }
    var previous: usize = 0;
    for (checker.rules) |item| {
        const order = rank(item.class) orelse
            return reject(diagnostic, checker.name, "rules follow class precedence", error.RuleOrder);
        if (item.class == .size or order < previous)
            return reject(diagnostic, checker.name, "rules follow class precedence", error.RuleOrder);
        previous = order;
        if (!scopeFits(checker, item.scope))
            return reject(diagnostic, checker.name, "rule scope fits the record", error.InvalidCheckScope);
        if ((item.scope == .selected) != (checker.selector != null))
            return reject(diagnostic, checker.name, "selected scope matches the selector", error.InvalidCheckScope);
        const extent = scopeExtent(checker, item.scope).len;
        const expected: Class = switch (item.check) {
            .magic => .magic,
            .version => .version,
            .seqlock, .nonzero, .entry_identity => .torn,
            .crc32 => .checksum,
            .zero, .payload_tail => .reserved,
            .member => .unknown_kind,
            .equal, .at_most => .range,
        };
        if (expected != item.class)
            return reject(diagnostic, checker.name, "check kind matches its class", error.RuleClass);
        const ok = switch (item.check) {
            .magic => |value| blk: {
                for (description.magics) |magic| {
                    if (magic.value == value.value) break :blk value.int.width == 8 and intFits(value.int, extent);
                }
                return reject(diagnostic, checker.name, "magic rule names a declared magic", error.UnknownRuleMagic);
            },
            .version => |value| value.value == description.abi_version and intFits(value.int, extent),
            .equal, .at_most => |value| intFits(value.int, extent) and valueFits(value.value, value.int.width),
            .seqlock, .nonzero => |value| intFits(value, extent),
            .zero => |value| fits(value.offset, value.len, extent),
            .crc32 => |value| blk: {
                if (value.crc.width != 4 or !fits(value.span.offset, value.span.len, extent) or
                    value.crc.offset < value.span.offset or !fits(value.crc.offset, 4, value.span.offset + value.span.len))
                    break :blk false;
                var last: usize = value.crc.offset + 4;
                for (value.zeroed) |zeroed| {
                    if (zeroed.offset < last or !fits(zeroed.offset, zeroed.len, value.span.offset + value.span.len)) break :blk false;
                    last = zeroed.offset + zeroed.len;
                }
                break :blk true;
            },
            .member => |value| blk: {
                const enumeration = findEnum(description, value.enumeration) orelse
                    return reject(diagnostic, checker.name, "member rule names a wire enum", error.UnknownRuleEnum);
                if (!enumeration.wire)
                    return reject(diagnostic, checker.name, "member rule names a wire enum", error.UnknownRuleEnum);
                break :blk intFits(value.int, extent) and enumeration.width == value.int.width;
            },
            .entry_identity => |value| item.scope == .window and item.scope.window.mode == .journal and
                value.generation.width == 8 and value.sequence.width == 8 and
                intFits(value.generation, extent) and intFits(value.sequence, extent) and
                fits(value.header_generation, 8, checker.size),
            .payload_tail => |value| intFits(value.length, extent) and fits(value.payload.offset, value.payload.len, extent),
        };
        if (!ok) return reject(diagnostic, checker.name, "rule field fits its scope and width", error.InvalidRuleField);
    }
}

/// Reserved and padding leaves of a record (first instance of each row).
fn reservedLeaves(description: model.Description, record: model.Record, base: usize, out: []Span, len: *usize) Error!void {
    for (record.fields) |field| {
        if (field.is_record) {
            const nested = findRecord(description, field.element) orelse return error.UnknownCheckedRecord;
            // Row rules repeat per row, so the first row stands for all.
            try reservedLeaves(description, nested, base + field.offset, out, len);
        } else if (isReservedName(field.name)) {
            if (len.* == out.len) return error.TooManyReservedLeaves;
            out[len.*] = .{ .offset = base + field.offset, .len = field.size };
            len.* += 1;
        }
    }
}

/// Every reserved span has a zero rule; every publication sequence a seqlock rule.
pub fn validateCoverage(list: []const Checker, description: model.Description, diagnostic: *abi.Diagnostic) Error!void {
    diagnostic.* = .{};
    for (list) |checker| {
        const record = findRecord(description, checker.record) orelse
            return reject(diagnostic, checker.name, "checked record exists", error.UnknownCheckedRecord);
        var leaves: [64]Span = undefined;
        var count: usize = 0;
        reservedLeaves(description, record, 0, &leaves, &count) catch |err|
            return reject(diagnostic, checker.record, "reserved fields are enumerable", err);
        for (leaves[0..count]) |leaf| {
            var covered = false;
            for (list) |other| {
                if (!std.mem.eql(u8, other.record, checker.record)) continue;
                for (other.rules) |item| {
                    if (item.check != .zero) continue;
                    const base = scopeExtent(other, item.scope).base;
                    if (base + item.check.zero.offset == leaf.offset and item.check.zero.len == leaf.len) covered = true;
                }
            }
            if (!covered) return reject(diagnostic, checker.record, "every reserved span has a zero rule", error.UncoveredReserved);
        }
    }
    for (abi.atomic_fields) |atomic| {
        // Acks, the bank index and the journal head are counters, not seqlocks.
        if (std.mem.endsWith(u8, atomic.field, "_ack") or std.mem.eql(u8, atomic.field, "active_bank") or
            std.mem.eql(u8, atomic.field, "published_seq")) continue;
        var found = false;
        for (list) |checker| {
            for (checker.rules) |item| {
                if (item.check != .seqlock) continue;
                const record = findRecord(description, checker.record) orelse continue;
                const base = scopeExtent(checker, item.scope).base;
                if (fieldAt(description, record, base + item.check.seqlock.offset)) |name| {
                    if (std.mem.eql(u8, name, atomic.field)) found = true;
                }
            }
        }
        if (!found) return reject(diagnostic, atomic.record, "publication sequence has a seqlock rule", error.MissingSeqlockRule);
    }
}

/// Scalar leaf at an absolute offset (first instance of rows).
fn fieldAt(description: model.Description, record: model.Record, offset: usize) ?[]const u8 {
    for (record.fields) |field| {
        if (offset < field.offset or offset >= field.offset + field.size) continue;
        if (!field.is_record) return if (offset == field.offset) field.name else null;
        const nested = findRecord(description, field.element) orelse return null;
        const stride = field.size / field.count;
        return fieldAt(description, nested, (offset - field.offset) % stride);
    }
    return null;
}

fn load(bytes: []const u8, at_offset: usize, width: usize) u64 {
    return switch (width) {
        1 => bytes[at_offset],
        2 => std.mem.readInt(u16, bytes[at_offset..][0..2], .little),
        4 => std.mem.readInt(u32, bytes[at_offset..][0..4], .little),
        8 => std.mem.readInt(u64, bytes[at_offset..][0..8], .little),
        else => unreachable,
    };
}

fn crcZeros(hasher: *std.hash.Crc32, len: usize) void {
    for (0..len) |_| hasher.update(&.{0});
}

/// Whether one check holds for the scope instance at `base`.
fn holds(check: Check, bytes: []const u8, base: usize, sequence: u64) bool {
    return switch (check) {
        .magic, .version, .equal => |value| load(bytes, base + value.int.offset, value.int.width) == value.value,
        .at_most => |value| load(bytes, base + value.int.offset, value.int.width) <= value.value,
        .seqlock => |value| blk: {
            const seq = load(bytes, base + value.offset, value.width);
            break :blk seq != 0 and seq & 1 == 0;
        },
        .nonzero => |value| load(bytes, base + value.offset, value.width) != 0,
        .zero => |value| std.mem.allEqual(u8, bytes[base + value.offset ..][0..value.len], 0),
        .member => |value| blk: {
            const raw = load(bytes, base + value.int.offset, value.int.width);
            if (value.reject_zero and raw == 0) break :blk false;
            const enumeration = findEnum(model.description, value.enumeration).?;
            for (enumeration.values) |declared| {
                if (declared.value == raw) break :blk true;
            }
            break :blk false;
        },
        .crc32 => |value| blk: {
            var hasher: std.hash.Crc32 = .init();
            var cursor = value.span.offset;
            const stored: Span = .{ .offset = value.crc.offset, .len = value.crc.width };
            for ([_][]const Span{ &.{stored}, value.zeroed }) |group| {
                for (group) |zeroed| {
                    hasher.update(bytes[base + cursor .. base + zeroed.offset]);
                    crcZeros(&hasher, zeroed.len);
                    cursor = zeroed.offset + zeroed.len;
                }
            }
            hasher.update(bytes[base + cursor .. base + value.span.offset + value.span.len]);
            break :blk hasher.final() == load(bytes, base + value.crc.offset, 4);
        },
        .entry_identity => |value| load(bytes, base + value.generation.offset, 8) == load(bytes, value.header_generation, 8) and
            load(bytes, base + value.sequence.offset, 8) == sequence,
        .payload_tail => |value| blk: {
            const kept = @min(load(bytes, base + value.length.offset, value.length.width), value.payload.len);
            break :blk std.mem.allEqual(u8, bytes[base + value.payload.offset + kept ..][0 .. value.payload.len - kept], 0);
        },
    };
}

fn ruleHolds(item: Rule, bytes: []const u8, selected: usize) bool {
    switch (item.scope) {
        .whole => return holds(item.check, bytes, 0, 0),
        .selected => return holds(item.check, bytes, selected, 0),
        .rows => |rows| {
            for (0..rows.count) |row| {
                if (!holds(item.check, bytes, rows.base + row * rows.stride, 0)) return false;
            }
            return true;
        },
        .window => |window| {
            const head = load(bytes, window.head_offset, 8);
            const live: usize = @intCast(@min(head, window.capacity));
            for (0..live) |n| {
                // Ring index i and journal sequence i + 1 share slot i % capacity.
                const index = head - live + n;
                const slot: usize = @intCast(index % window.capacity);
                if (!holds(item.check, bytes, window.base + slot * window.stride, index + 1)) return false;
            }
            return true;
        },
    }
}

/// Zig reference for the generated checkers; `bank` is ignored without a selector.
pub fn verdict(checker: Checker, bytes: []const u8, bank: usize) Class {
    if (bytes.len != checker.size) return .size;
    var selected: usize = 0;
    if (checker.selector) |rows| {
        if (bank >= rows.count) return .range;
        selected = rows.base + bank * rows.stride;
    }
    // Rules are sorted by class, so the first failure is the highest-precedence one.
    for (checker.rules) |item| {
        if (!ruleHolds(item, bytes, selected)) return item.class;
    }
    return .ok;
}

pub fn validateChecks(list: []const Checker, description: model.Description, diagnostic: *abi.Diagnostic) Error!void {
    for (list, 0..) |checker, i| {
        for (list[0..i]) |prior| {
            if (std.mem.eql(u8, prior.name, checker.name))
                return reject(diagnostic, checker.name, "unique checker names", error.DuplicateChecker);
        }
        try validateChecker(checker, description, diagnostic);
    }
    try validateCoverage(list, description, diagnostic);
}

pub fn validate(diagnostic: *abi.Diagnostic) Error!void {
    try validateChecks(&checkers, model.description, diagnostic);
}

test "check tables are valid and cover every reserved span" {
    var diagnostic: abi.Diagnostic = .{};
    try validate(&diagnostic);
    try std.testing.expectEqualStrings("", diagnostic.path);
    try std.testing.expectEqual(@as(usize, 9), checkers.len);
    try std.testing.expectEqual(@as(usize, 5048), status_events.window.base);
    try std.testing.expectEqual(@as(usize, 64), spec_bank.selected.base);
    try std.testing.expectEqual(@as(usize, 2016), spec_bank.selected.stride);
    try std.testing.expectEqual(@as(usize, 20), bank_crc_zeroed[0].offset);
}

test "rules out of precedence order are rejected" {
    var diagnostic: abi.Diagnostic = .{};
    const swapped = [_]Rule{ ctl_command_rules[2], ctl_command_rules[0] };
    const bad: Checker = .{ .name = "fixture", .record = "CtlCommand", .size = 64, .rules = &swapped };
    try std.testing.expectError(error.RuleOrder, validateChecker(bad, model.description, &diagnostic));
    try std.testing.expectEqualStrings("fixture", diagnostic.path);
    try std.testing.expectEqualStrings("rules follow class precedence", diagnostic.invariant);
}

test "member rules must name a wire enum of the field width" {
    var diagnostic: abi.Diagnostic = .{};
    var rules = ctl_command_rules;
    rules[4].check.member.enumeration = "SlotEvent";
    const internal: Checker = .{ .name = "fixture", .record = "CtlCommand", .size = 64, .rules = &rules };
    try std.testing.expectError(error.UnknownRuleEnum, validateChecker(internal, model.description, &diagnostic));
    try std.testing.expectEqualStrings("member rule names a wire enum", diagnostic.invariant);

    rules[4].check.member.enumeration = "SlotPhase";
    const wide: Checker = .{ .name = "fixture", .record = "CtlCommand", .size = 64, .rules = &rules };
    try std.testing.expectError(error.InvalidRuleField, validateChecker(wide, model.description, &diagnostic));
    try std.testing.expectEqualStrings("rule field fits its scope and width", diagnostic.invariant);
}

test "an uncovered reserved span is rejected" {
    var diagnostic: abi.Diagnostic = .{};
    const partial = [_]Rule{ ctl_command_rules[0], ctl_command_rules[1], ctl_command_rules[2], ctl_command_rules[4] };
    const list = [_]Checker{.{ .name = "fixture", .record = "CtlCommand", .size = 64, .rules = &partial }};
    try validateChecker(list[0], model.description, &diagnostic);
    try std.testing.expectError(error.UncoveredReserved, validateCoverage(&list, model.description, &diagnostic));
    try std.testing.expectEqualStrings("CtlCommand", diagnostic.path);
    try std.testing.expectEqualStrings("every reserved span has a zero rule", diagnostic.invariant);
}

test "a missing seqlock rule is rejected" {
    var diagnostic: abi.Diagnostic = .{};
    var list = checkers;
    list[0].rules = root_status_rules[0..2] ++ root_status_rules[3..];
    try std.testing.expectError(error.MissingSeqlockRule, validateCoverage(&list, model.description, &diagnostic));
    try std.testing.expectEqualStrings("RootStatusHeader", diagnostic.path);
}

test "a rule outside its scope is rejected" {
    var diagnostic: abi.Diagnostic = .{};
    const outside = [_]Rule{rule(.reserved, status_rows, .{ .zero = .{ .offset = 78, .len = 4 } })};
    const bad: Checker = .{ .name = "fixture", .record = "RootStatusPage", .size = 16384, .rules = &outside };
    try std.testing.expectError(error.InvalidRuleField, validateChecker(bad, model.description, &diagnostic));
}

pub const samples = @import("samples.zig");

// Verdict cases live in vectors.zon; generate_vectors.zig runs them through verdict().

test "every checker accepts its sample" {
    const allocator = std.testing.allocator;
    for (checkers) |checker| {
        const bytes = try samples.image(allocator, checker.name);
        defer allocator.free(bytes);
        try std.testing.expectEqual(Class.ok, verdict(checker, bytes, 0));
    }
}
