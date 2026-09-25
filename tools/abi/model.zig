//! Serde-free reflection of the typed orchestration wire contract.
const std = @import("std");
const abi = @import("orchestrator_abi");

pub const Field = struct {
    name: []const u8,
    offset: usize,
    size: usize,
    type: []const u8,
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

fn describeRecord(comptime T: type) Record {
    const info = @typeInfo(T).@"struct";
    const fields = comptime blk: {
        var result: [info.fields.len]Field = undefined;
        for (info.fields, 0..) |field, i| {
            result[i] = .{
                .name = field.name,
                .offset = @offsetOf(T, field.name),
                .size = @sizeOf(field.type),
                .type = @typeName(field.type),
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
    },
    .records = &records,
    .enums = &enums,
    .transitions = &transitions,
    .atomic_fields = &abi.atomic_fields,
};

pub fn validateRecord(record: Record, diagnostic: *abi.Diagnostic) !void {
    diagnostic.* = .{};
    var next_offset: usize = 0;
    for (record.fields) |field| {
        if (field.offset != next_offset) {
            diagnostic.* = .{ .path = record.name, .invariant = "no implicit field padding" };
            return error.InvalidRecordOffset;
        }
        next_offset = try std.math.add(usize, next_offset, field.size);
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
                    field.offset % atomic.width != 0)
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

pub fn validate(diagnostic: *abi.Diagnostic) !void {
    diagnostic.* = .{};
    for (description.records) |record| try validateRecord(record, diagnostic);
    for (description.enums) |enumeration| try validateEnum(enumeration, diagnostic);
    if (description.atomic_fields.len != 7) {
        diagnostic.* = .{ .path = "atomic_fields", .invariant = "all publication fields listed" };
        return error.MissingAtomicField;
    }
    try validateAtomicFields(description.atomic_fields, description.records, diagnostic);
    for (description.magics, 0..) |magic, i| {
        if (magic.bytes.len != 8) {
            diagnostic.* = .{ .path = magic.name, .invariant = "eight magic bytes" };
            return error.InvalidMagic;
        }
        for (description.magics[0..i]) |prior| {
            if (magic.value == prior.value) {
                diagnostic.* = .{ .path = magic.name, .invariant = "distinct magics" };
                return error.DuplicateMagic;
            }
        }
    }
}

test "reflection describes pinned bank and journal geometry" {
    try std.testing.expectEqual(@as(usize, 8), description.magics.len);
    try std.testing.expectEqual(@as(usize, 16), description.records.len);
    try std.testing.expectEqual(@as(usize, 2016), description.records[5].size);
    try std.testing.expectEqual(@as(usize, 20), description.records[5].fields[4].offset);
    try std.testing.expectEqual(@as(usize, 4096), description.records[15].size);
    try std.testing.expectEqual(@as(usize, 17), description.transitions.len);
    try std.testing.expect(!description.enums[11].wire);
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

    const bad_fields = [_]Field{.{ .name = "field", .offset = 1, .size = 4, .type = "u32" }};
    try std.testing.expectError(error.InvalidRecordOffset, validateRecord(.{
        .name = "fixture_record",
        .size = 4,
        .alignment = 4,
        .fields = &bad_fields,
    }, &diagnostic));
    try std.testing.expectEqualStrings("no implicit field padding", diagnostic.invariant);

    const sized_fields = [_]Field{.{ .name = "field", .offset = 0, .size = 4, .type = "u32" }};
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
