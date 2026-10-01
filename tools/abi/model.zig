//! Serde-free reflection of the typed orchestration wire contract.
const std = @import("std");
const abi = @import("orchestrator_abi");

pub const Field = struct {
    name: []const u8,
    offset: usize,
    size: usize,
    type: []const u8,
    // Structured form of `type` for generators: a scalar name (u8, u16, u32,
    // u64) or a record's short name, repeated `count` times.
    element: []const u8,
    count: usize,
    is_record: bool,
};

pub const Record = struct {
    name: []const u8,
    size: usize,
    alignment: usize,
    fields: []const Field,
};

pub const EnumValue = struct {
    name: []const u8,
    value: u64,
};

pub const Enum = struct {
    name: []const u8,
    width: usize,
    wire: bool,
    values: []const EnumValue,
};

pub const Transition = struct {
    from: []const u8,
    to: []const u8,
    event: []const u8,
};

pub const Magic = struct {
    name: []const u8,
    bytes: []const u8,
    value: u64,
};

pub const Constants = struct {
    child_count: usize,
    event_count: usize,
    journal_capacity: usize,
    journal_payload_size: usize,
    /// RootEvent.child of a global (not per-child) event.
    root_event_no_child: u8,
    /// Row and event flag bits.
    fault_mr0_valid: u16,
    fault_mr1_valid: u16,
    down_interval_open: u16,
};

pub const Description = struct {
    abi_version: u32,
    magics: []const Magic,
    constants: Constants,
    records: []const Record,
    enums: []const Enum,
    transitions: []const Transition,
    atomic_fields: []const abi.AtomicField,
};

const magics = [_]Magic{
    .{ .name = "status", .bytes = "CHRYSTA1", .value = abi.magic.status },
    .{ .name = "spec", .bytes = "CHRYSPE1", .value = abi.magic.spec },
    .{ .name = "command", .bytes = "CHRYCMD1", .value = abi.magic.command },
    .{ .name = "reply", .bytes = "CHRYREP1", .value = abi.magic.reply },
    .{ .name = "worker_identity", .bytes = "CHRYWID1", .value = abi.magic.worker_identity },
    .{ .name = "worker_status", .bytes = "CHRYWST1", .value = abi.magic.worker_status },
    .{ .name = "request", .bytes = "CHRYREQ1", .value = abi.magic.request },
    .{ .name = "completion", .bytes = "CHRYCMP1", .value = abi.magic.completion },
};

fn shortName(comptime T: type) []const u8 {
    const full = @typeName(T);
    const dot = std.mem.lastIndexOfScalar(u8, full, '.') orelse return full;
    return full[dot + 1 ..];
}

const ElementType = struct {
    element: []const u8,
    count: usize,
    is_record: bool,
};

fn elementType(comptime T: type) ElementType {
    return switch (@typeInfo(T)) {
        .int => |int| blk: {
            if (int.signedness != .unsigned or (int.bits != 8 and int.bits != 16 and int.bits != 32 and int.bits != 64))
                @compileError("wire scalars are u8, u16, u32 or u64: " ++ @typeName(T));
            break :blk .{ .element = @typeName(T), .count = 1, .is_record = false };
        },
        .@"struct" => .{ .element = shortName(T), .count = 1, .is_record = true },
        .array => |array| blk: {
            const inner = elementType(array.child);
            if (inner.count != 1) @compileError("nested wire arrays are not supported: " ++ @typeName(T));
            break :blk .{ .element = inner.element, .count = array.len, .is_record = inner.is_record };
        },
        else => @compileError("unsupported wire field type " ++ @typeName(T)),
    };
}

fn describeRecord(comptime T: type) Record {
    const info = @typeInfo(T).@"struct";
    const fields = comptime blk: {
        var result: [info.fields.len]Field = undefined;
        for (info.fields, 0..) |field, i| {
            const element = elementType(field.type);
            result[i] = .{
                .name = field.name,
                .offset = @offsetOf(T, field.name),
                .size = @sizeOf(field.type),
                .type = @typeName(field.type),
                .element = element.element,
                .count = element.count,
                .is_record = element.is_record,
            };
        }
        break :blk result;
    };
    return .{
        .name = shortName(T),
        .size = @sizeOf(T),
        .alignment = @alignOf(T),
        .fields = &fields,
    };
}

fn describeEnum(comptime T: type, comptime wire: bool) Enum {
    const info = @typeInfo(T).@"enum";
    const values = comptime blk: {
        var result: [info.fields.len]EnumValue = undefined;
        for (info.fields, 0..) |field, i| {
            result[i] = .{ .name = field.name, .value = field.value };
        }
        break :blk result;
    };
    return .{
        .name = shortName(T),
        .width = @sizeOf(T),
        .wire = wire,
        .values = &values,
    };
}

const records = [_]Record{
    describeRecord(abi.RootStatusHeader),
    describeRecord(abi.RootChildStatus),
    describeRecord(abi.RootEvent),
    describeRecord(abi.RootStatusPage),
    describeRecord(abi.SpecHeader),
    describeRecord(abi.SpecBank),
    describeRecord(abi.SpecPage),
    describeRecord(abi.CtlCommand),
    describeRecord(abi.CtlReply),
    describeRecord(abi.WorkerIdentityHeader),
    describeRecord(abi.WorkerIdentity),
    describeRecord(abi.WorkerStatusHeader),
    describeRecord(abi.WorkerStatus),
    describeRecord(abi.JournalHeader),
    describeRecord(abi.JournalEntry),
    describeRecord(abi.JournalPage),
};

const enums = [_]Enum{
    describeEnum(abi.RootChildWireState, true),
    describeEnum(abi.RootDesired, true),
    describeEnum(abi.RootEventKind, true),
    describeEnum(abi.RootGiveupReason, true),
    describeEnum(abi.RootControlKind, true),
    describeEnum(abi.PpOpcode, true),
    describeEnum(abi.CtlResult, true),
    describeEnum(abi.WorkerHealth, true),
    describeEnum(abi.RequestKind, true),
    describeEnum(abi.CompletionKind, true),
    describeEnum(abi.CompletionStatus, true),
    describeEnum(abi.SlotPhase, true),
    describeEnum(abi.SlotEvent, false),
    describeEnum(abi.AbiReject, false),
};

const transitions = blk: {
    var result: [abi.slot_transitions.len]Transition = undefined;
    for (abi.slot_transitions, 0..) |transition, i| {
        result[i] = .{
            .from = @tagName(transition.from),
            .to = @tagName(transition.to),
            .event = @tagName(transition.event),
        };
    }
    break :blk result;
};

pub const description: Description = .{
    .abi_version = abi.abi_version,
    .magics = &magics,
    .constants = .{
        .child_count = abi.child_count,
        .event_count = abi.event_count,
        .journal_capacity = abi.journal_capacity,
        .journal_payload_size = abi.journal_payload_size,
        .root_event_no_child = abi.root_event_no_child,
        .fault_mr0_valid = abi.fault_mr0_valid,
        .fault_mr1_valid = abi.fault_mr1_valid,
        .down_interval_open = abi.down_interval_open,
    },
    .records = &records,
    .enums = &enums,
    .transitions = &transitions,
    .atomic_fields = &abi.atomic_fields,
};

const Digest = struct {
    hasher: std.hash.Crc32 = .init(),

    fn text(self: *Digest, bytes: []const u8) void {
        self.hasher.update(bytes);
        self.hasher.update(&.{0}); // terminator keeps adjacent names distinct
    }

    fn number(self: *Digest, value: u64) void {
        var bytes: [8]u8 = undefined;
        std.mem.writeInt(u64, &bytes, value, .little);
        self.hasher.update(&bytes);
    }
};

/// CRC-32 over a canonical rendering of the wire contract. Every generated codec carries it,
/// so peers built from different models can refuse each other at HELLO.
pub fn layoutDigest(value: Description) u32 {
    var digest: Digest = .{};
    digest.number(value.abi_version);
    inline for (std.meta.fields(Constants)) |field| digest.number(@field(value.constants, field.name));
    digest.number(value.magics.len);
    for (value.magics) |magic| {
        digest.text(magic.name);
        digest.number(magic.value);
    }
    digest.number(value.records.len);
    for (value.records) |record| {
        digest.text(record.name);
        digest.number(record.size);
        digest.number(record.alignment);
        digest.number(record.fields.len);
        for (record.fields) |field| {
            digest.text(field.name);
            digest.number(field.offset);
            digest.number(field.size);
            digest.text(field.element);
            digest.number(field.count);
        }
    }
    digest.number(value.enums.len);
    for (value.enums) |enumeration| {
        digest.text(enumeration.name);
        digest.number(enumeration.width);
        digest.number(enumeration.values.len);
        for (enumeration.values) |item| {
            digest.text(item.name);
            digest.number(item.value);
        }
    }
    digest.number(value.transitions.len);
    for (value.transitions) |transition| {
        digest.text(transition.from);
        digest.text(transition.to);
        digest.text(transition.event);
    }
    digest.number(value.atomic_fields.len);
    for (value.atomic_fields) |atomic| {
        digest.text(atomic.record);
        digest.text(atomic.field);
        digest.number(atomic.width);
    }
    return digest.hasher.final();
}

pub fn validateRecord(record: Record, diagnostic: *abi.Diagnostic) !void {
    diagnostic.* = .{};
    var next_offset: usize = 0;
    for (record.fields) |field| {
        if (field.offset != next_offset) {
            diagnostic.* = .{ .path = record.name, .invariant = "no implicit field padding" };
            return error.InvalidRecordOffset;
        }
        next_offset = std.math.add(usize, next_offset, field.size) catch {
            diagnostic.* = .{ .path = record.name, .invariant = "field extent does not overflow" };
            return error.InvalidRecordSize;
        };
    }
    if (next_offset != record.size) {
        diagnostic.* = .{ .path = record.name, .invariant = "exact record size" };
        return error.InvalidRecordSize;
    }
}

pub fn validateEnum(enumeration: Enum, diagnostic: *abi.Diagnostic) !void {
    diagnostic.* = .{};
    if (enumeration.width != 1 and enumeration.width != 2 and enumeration.width != 4 and enumeration.width != 8) {
        diagnostic.* = .{ .path = enumeration.name, .invariant = "supported enum width" };
        return error.InvalidEnumWidth;
    }
    for (enumeration.values, 0..) |value, i| {
        if (enumeration.width < 8 and value.value >= (@as(u64, 1) << @as(u6, @intCast(enumeration.width * 8)))) {
            diagnostic.* = .{ .path = enumeration.name, .invariant = "value fits declared width" };
            return error.EnumValueOutOfRange;
        }
        for (enumeration.values[0..i]) |prior| {
            if (value.value == prior.value or std.mem.eql(u8, value.name, prior.name)) {
                diagnostic.* = .{ .path = enumeration.name, .invariant = "unique enum name and value" };
                return error.DuplicateEnumValue;
            }
        }
    }
}

pub fn validateAtomicFields(fields: []const abi.AtomicField, record_list: []const Record, diagnostic: *abi.Diagnostic) !void {
    diagnostic.* = .{};
    for (fields, 0..) |atomic, i| {
        var found = false;
        for (record_list) |record| {
            if (!std.mem.eql(u8, atomic.record, record.name)) continue;
            for (record.fields) |field| {
                if (!std.mem.eql(u8, atomic.field, field.name)) continue;
                found = true;
                if ((atomic.width != 4 and atomic.width != 8) or field.size != atomic.width or
                    field.offset % atomic.width != 0 or record.alignment % atomic.width != 0)
                {
                    diagnostic.* = .{ .path = atomic.record, .invariant = "atomic width and alignment" };
                    return error.InvalidAtomicField;
                }
            }
        }
        if (!found) {
            diagnostic.* = .{ .path = atomic.record, .invariant = "atomic field exists" };
            return error.UnknownAtomicField;
        }
        for (fields[0..i]) |prior| {
            if (std.mem.eql(u8, atomic.record, prior.record) and std.mem.eql(u8, atomic.field, prior.field)) {
                diagnostic.* = .{ .path = atomic.record, .invariant = "unique atomic field" };
                return error.DuplicateAtomicField;
            }
        }
    }
}

pub fn validateDescription(value: Description, diagnostic: *abi.Diagnostic) !void {
    diagnostic.* = .{};
    if (value.abi_version != abi.abi_version or value.constants.child_count != abi.child_count or
        value.constants.event_count != abi.event_count or
        value.constants.journal_capacity != abi.journal_capacity or
        value.constants.journal_payload_size != abi.journal_payload_size or
        value.constants.root_event_no_child != abi.root_event_no_child or
        value.constants.fault_mr0_valid != abi.fault_mr0_valid or
        value.constants.fault_mr1_valid != abi.fault_mr1_valid or
        value.constants.down_interval_open != abi.down_interval_open)
    {
        diagnostic.* = .{ .path = "constants", .invariant = "typed ABI version and capacities" };
        return error.InvalidModelConstants;
    }
    if (value.records.len != 16 or value.enums.len != 14 or
        value.magics.len != 8 or value.transitions.len != abi.slot_transitions.len)
    {
        diagnostic.* = .{ .path = "description", .invariant = "complete wire metadata" };
        return error.IncompleteModel;
    }
    for (value.records) |record| try validateRecord(record, diagnostic);
    for (value.enums) |enumeration| try validateEnum(enumeration, diagnostic);
    if (value.atomic_fields.len != 7) {
        diagnostic.* = .{ .path = "atomic_fields", .invariant = "all publication fields listed" };
        return error.MissingAtomicField;
    }
    try validateAtomicFields(value.atomic_fields, value.records, diagnostic);
    for (abi.atomic_fields) |required| {
        var found = false;
        for (value.atomic_fields) |field| {
            if (std.mem.eql(u8, field.record, required.record) and
                std.mem.eql(u8, field.field, required.field) and field.width == required.width)
            {
                found = true;
                break;
            }
        }
        if (!found) {
            diagnostic.* = .{ .path = required.record, .invariant = "required publication field and width" };
            return error.MissingAtomicField;
        }
    }
    for (value.magics, 0..) |magic, i| {
        if (magic.bytes.len != 8) {
            diagnostic.* = .{ .path = magic.name, .invariant = "eight magic bytes" };
            return error.InvalidMagic;
        }
        for (value.magics[0..i]) |prior| {
            if (magic.value == prior.value) {
                diagnostic.* = .{ .path = magic.name, .invariant = "distinct magics" };
                return error.DuplicateMagic;
            }
        }
    }
}

pub fn validate(diagnostic: *abi.Diagnostic) !void {
    try validateDescription(description, diagnostic);
}

test "reflection describes pinned bank and journal geometry" {
    try std.testing.expectEqual(@as(usize, 8), description.magics.len);
    try std.testing.expectEqual(@as(usize, 16), description.records.len);
    try std.testing.expectEqual(@as(usize, 2016), description.records[5].size);
    try std.testing.expectEqual(@as(usize, 20), description.records[5].fields[4].offset);
    try std.testing.expectEqual(@as(usize, 4096), description.records[15].size);
    try std.testing.expectEqual(@as(usize, 17), description.transitions.len);
    try std.testing.expect(!description.enums[13].wire);
    const children = description.records[3].fields[1];
    try std.testing.expectEqualStrings("children", children.name);
    try std.testing.expectEqualStrings("RootChildStatus", children.element);
    try std.testing.expectEqual(@as(usize, 62), children.count);
    try std.testing.expect(children.is_record);
    const budget = description.records[5].fields[5];
    try std.testing.expectEqualStrings("budget", budget.name);
    try std.testing.expectEqualStrings("u32", budget.element);
    try std.testing.expectEqual(@as(usize, 62), budget.count);
    try std.testing.expect(!budget.is_record);
    var diagnostic: abi.Diagnostic = .{};
    try validate(&diagnostic);
}

test "model rejects enum collisions and malformed layout metadata" {
    var diagnostic: abi.Diagnostic = .{};
    const bad_values = [_]EnumValue{
        .{ .name = "one", .value = 1 },
        .{ .name = "two", .value = 1 },
    };
    try std.testing.expectError(error.DuplicateEnumValue, validateEnum(.{
        .name = "fixture_enum",
        .width = 1,
        .wire = true,
        .values = &bad_values,
    }, &diagnostic));
    try std.testing.expectEqualStrings("fixture_enum", diagnostic.path);

    const out_of_range = [_]EnumValue{.{ .name = "too_large", .value = 256 }};
    try std.testing.expectError(error.EnumValueOutOfRange, validateEnum(.{
        .name = "fixture_enum",
        .width = 1,
        .wire = true,
        .values = &out_of_range,
    }, &diagnostic));
    try std.testing.expectEqualStrings("value fits declared width", diagnostic.invariant);

    const bad_fields = [_]Field{.{ .name = "field", .offset = 1, .size = 4, .type = "u32", .element = "u32", .count = 1, .is_record = false }};
    try std.testing.expectError(error.InvalidRecordOffset, validateRecord(.{
        .name = "fixture_record",
        .size = 4,
        .alignment = 4,
        .fields = &bad_fields,
    }, &diagnostic));
    try std.testing.expectEqualStrings("no implicit field padding", diagnostic.invariant);

    const sized_fields = [_]Field{.{ .name = "field", .offset = 0, .size = 4, .type = "u32", .element = "u32", .count = 1, .is_record = false }};
    try std.testing.expectError(error.InvalidRecordSize, validateRecord(.{
        .name = "fixture_page",
        .size = 4096,
        .alignment = 4,
        .fields = &sized_fields,
    }, &diagnostic));
    try std.testing.expectEqualStrings("exact record size", diagnostic.invariant);

    const bad_atomic = [_]abi.AtomicField{.{ .record = "SpecBank", .field = "bank_seq", .width = 8 }};
    try std.testing.expectError(error.InvalidAtomicField, validateAtomicFields(&bad_atomic, description.records, &diagnostic));
    try std.testing.expectEqualStrings("atomic width and alignment", diagnostic.invariant);
}

test "layout digest is stable and tracks every layout fact" {
    const digest = layoutDigest(description);
    try std.testing.expectEqual(digest, layoutDigest(description));

    var moved_records: [description.records.len]Record = undefined;
    @memcpy(&moved_records, description.records);
    var moved_fields: [description.records[5].fields.len]Field = undefined;
    @memcpy(&moved_fields, description.records[5].fields);
    moved_fields[4].offset += 4;
    moved_records[5].fields = &moved_fields;
    var moved = description;
    moved.records = &moved_records;
    try std.testing.expect(layoutDigest(moved) != digest);

    var renamed = description;
    renamed.abi_version += 1;
    try std.testing.expect(layoutDigest(renamed) != digest);
}
