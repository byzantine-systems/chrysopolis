//! Shared golden vectors: wire images built from the typed model, each with the verdict both
//! generated codecs must reach. `vectors.zon` holds the described cases; publication.zig's
//! writers add a snapshot after every protocol step; sweeps over the rule tables add every
//! reserved span, every undeclared enum value and every record size. No generated codec builds
//! an image, and a vector whose reference verdict differs from its stated one is refused.
//! Output goes to a build directory, never the tree: `<name>.bin` per vector plus
//! `manifest.txt`, a line format C reads without a parser library.
const std = @import("std");
const abi = @import("orchestrator_abi");
const model = @import("model");
const checks = @import("checks");
const publication = @import("publication");

const samples = checks.samples;
const Class = checks.Class;
const Writer = std.Io.Writer;

/// Where a vector's image starts.
pub const Base = enum {
    /// The valid sample for the checker.
    sample,
    zeroes,
    /// A full request journal whose newest sequence is the u64 maximum.
    journal_max,
    /// A spec page with both banks published.
    spec_two_banks,
    /// `hex`: a pinned wire image; the rest of the record is zero.
    hex,
};

pub const Edit = union(enum) {
    /// A little-endian value at a scalar field's width.
    set: Set,
    /// A wrapping add at a scalar field's width.
    add: Set,
    /// One byte inside a field.
    byte: Byte,
    /// Grows (+1) or shrinks (-1) the image.
    resize: i8,

    pub const Set = struct { field: []const u8, value: u64 };
    pub const Byte = struct { field: []const u8, index: usize = 0, value: u8 };
};

pub const Vector = struct {
    name: []const u8,
    checker: []const u8,
    base: Base = .sample,
    hex: []const u8 = "",
    /// Spec bank index; ignored by checkers without a selector.
    bank: usize = 0,
    edits: []const Edit = &.{},
    /// Recompute both spec banks' CRCs after the edits.
    reseal: bool = false,
    expect: Class,
};

pub const Image = struct {
    name: []const u8,
    checker: []const u8,
    bank: usize,
    expect: Class,
    /// Every reserved byte is zero, so decoding then encoding reproduces the image.
    canonical: bool,
    bytes: []const u8,
};

pub const Error = error{
    UnknownChecker,
    UnknownField,
    IndexOutOfRange,
    NotScalar,
    BadBase,
    BadHex,
    BadName,
    BadResize,
    DuplicateName,
    WrongVerdict,
    StepNotInProtocol,
};

pub const authored: []const Vector = @import("vectors.zon");

comptime {
    // A typo in vectors.zon is a compile error, never a skipped vector.
    @setEvalBranchQuota(2_000_000);
    for (authored, 0..) |vector, i| {
        validate(vector) catch |err| @compileError(vector.name ++ ": " ++ @errorName(err));
        for (authored[0..i]) |prior| {
            if (std.mem.eql(u8, prior.name, vector.name)) @compileError("duplicate vector " ++ vector.name);
        }
    }
}

/// Byte span of a scalar leaf: `width` is the element size, `len` the whole leaf.
pub const Location = struct { offset: usize, width: usize, len: usize };

/// Resolves `header.seq` or `banks[1].budget[3]` against a record of the model.
pub fn locate(record_name: []const u8, path: []const u8) Error!Location {
    var record = checks.findRecord(model.description, record_name) orelse return error.UnknownField;
    var offset: usize = 0;
    var parts = std.mem.splitScalar(u8, path, '.');
    while (parts.next()) |part| {
        const open = std.mem.indexOfScalar(u8, part, '[');
        const name = part[0 .. open orelse part.len];
        const index: ?usize = if (open) |at| blk: {
            if (part[part.len - 1] != ']') return error.UnknownField;
            break :blk std.fmt.parseInt(usize, part[at + 1 .. part.len - 1], 10) catch return error.UnknownField;
        } else null;
        const field = for (record.fields) |field| {
            if (std.mem.eql(u8, field.name, name)) break field;
        } else return error.UnknownField;
        const stride = field.size / field.count;
        if (index) |i| {
            if (i >= field.count) return error.IndexOutOfRange;
        } else if (field.count != 1 and field.is_record) return error.UnknownField;
        offset += field.offset + (index orelse 0) * stride;
        if (field.is_record) {
            record = checks.findRecord(model.description, field.element) orelse return error.UnknownField;
            continue;
        }
        if (parts.peek() != null) return error.UnknownField;
        return .{ .offset = offset, .width = stride, .len = if (index != null) stride else field.size };
    }
    // The path named a record, not a leaf.
    return error.UnknownField;
}

fn scalar(record: []const u8, path: []const u8) Error!Location {
    const location = try locate(record, path);
    if (location.len != location.width) return error.NotScalar;
    return location;
}

fn validName(name: []const u8) bool {
    if (name.len == 0) return false;
    for (name) |c| {
        if (!std.ascii.isLower(c) and !std.ascii.isDigit(c) and c != '_') return false;
    }
    return true;
}

/// Bytes a pinned hex image spells; whitespace is ignored.
fn hexLength(text: []const u8) Error!usize {
    var digits: usize = 0;
    for (text) |c| {
        if (std.ascii.isWhitespace(c)) continue;
        if (!std.ascii.isHex(c)) return error.BadHex;
        digits += 1;
    }
    if (digits % 2 != 0) return error.BadHex;
    return digits / 2;
}

/// Everything about a vector that does not need its bytes. Runs at comptime over vectors.zon.
pub fn validate(vector: Vector) Error!void {
    if (!validName(vector.name)) return error.BadName;
    const checker = checks.findChecker(&checks.checkers, vector.checker) orelse return error.UnknownChecker;
    switch (vector.base) {
        .sample, .zeroes => {},
        .journal_max => if (!std.mem.eql(u8, checker.record, "JournalPage")) return error.BadBase,
        .spec_two_banks => if (!std.mem.eql(u8, checker.record, "SpecPage")) return error.BadBase,
        .hex => if (try hexLength(vector.hex) > checker.size) return error.BadHex,
    }
    if (vector.base != .hex and vector.hex.len != 0) return error.BadBase;
    if (vector.reseal and !std.mem.eql(u8, checker.record, "SpecPage")) return error.BadBase;
    for (vector.edits) |edit| switch (edit) {
        .set, .add => |set| {
            const location = try scalar(checker.record, set.field);
            if (location.width < 8 and set.value >> @intCast(location.width * 8) != 0) return error.NotScalar;
        },
        .byte => |byte| if (byte.index >= (try locate(checker.record, byte.field)).len) return error.IndexOutOfRange,
        .resize => |delta| if (delta != 1 and delta != -1) return error.BadResize,
    };
}

fn load(bytes: []const u8, at: usize, width: usize) u64 {
    return switch (width) {
        1 => bytes[at],
        2 => std.mem.readInt(u16, bytes[at..][0..2], .little),
        4 => std.mem.readInt(u32, bytes[at..][0..4], .little),
        8 => std.mem.readInt(u64, bytes[at..][0..8], .little),
        else => unreachable, // model scalars are u8, u16, u32 or u64
    };
}

fn store(bytes: []u8, at: usize, width: usize, value: u64) void {
    switch (width) {
        1 => bytes[at] = @truncate(value),
        2 => std.mem.writeInt(u16, bytes[at..][0..2], @truncate(value), .little),
        4 => std.mem.writeInt(u32, bytes[at..][0..4], @truncate(value), .little),
        8 => std.mem.writeInt(u64, bytes[at..][0..8], value, .little),
        else => unreachable, // model scalars are u8, u16, u32 or u64
    }
}

fn sample(arena: std.mem.Allocator, checker: checks.Checker) (Error || std.mem.Allocator.Error)![]u8 {
    return samples.image(arena, checker.name) catch |err| switch (err) {
        error.OutOfMemory => error.OutOfMemory,
        error.NoSample => error.UnknownChecker,
    };
}

fn baseImage(arena: std.mem.Allocator, vector: Vector, checker: checks.Checker) (Error || std.mem.Allocator.Error)![]u8 {
    switch (vector.base) {
        .sample => return sample(arena, checker),
        .journal_max => return arena.dupe(u8, std.mem.asBytes(&samples.journalAtMax())),
        .spec_two_banks => return arena.dupe(u8, std.mem.asBytes(&samples.specTwoBanks())),
        .zeroes, .hex => {
            const bytes = try arena.alloc(u8, checker.size);
            @memset(bytes, 0);
            var digits: usize = 0;
            for (vector.hex) |c| {
                if (std.ascii.isWhitespace(c)) continue;
                const nibble = std.fmt.charToDigit(c, 16) catch return error.BadHex;
                bytes[digits / 2] |= if (digits % 2 == 0) nibble << 4 else nibble;
                digits += 1;
            }
            return bytes;
        },
    }
}

fn resealBanks(bytes: []u8) void {
    const base = comptime checks.at(abi.SpecPage, "banks");
    for (0..2) |i| {
        const at = base + i * @sizeOf(abi.SpecBank);
        var bank = std.mem.bytesToValue(abi.SpecBank, bytes[at..][0..@sizeOf(abi.SpecBank)]);
        samples.sealBank(&bank);
        @memcpy(bytes[at..][0..@sizeOf(abi.SpecBank)], std.mem.asBytes(&bank));
    }
}

fn isReservedName(name: []const u8) bool {
    return std.mem.startsWith(u8, name, "reserved") or std.mem.startsWith(u8, name, "pad");
}

/// Every reserved leaf of every row is zero.
fn canonicalAt(record: model.Record, bytes: []const u8, base: usize) bool {
    for (record.fields) |field| {
        if (field.is_record) {
            const nested = checks.findRecord(model.description, field.element).?;
            const stride = field.size / field.count;
            for (0..field.count) |row| {
                if (!canonicalAt(nested, bytes, base + field.offset + row * stride)) return false;
            }
        } else if (isReservedName(field.name) and !std.mem.allEqual(u8, bytes[base + field.offset ..][0..field.size], 0)) {
            return false;
        }
    }
    return true;
}

fn canonical(checker: checks.Checker, bytes: []const u8) bool {
    if (bytes.len != checker.size) return false;
    return canonicalAt(checks.findRecord(model.description, checker.record).?, bytes, 0);
}

/// Checks the stated verdict against the reference; on a mismatch `diagnostic` names both.
fn finish(arena: std.mem.Allocator, checker: checks.Checker, name: []const u8, bank: usize, expect: Class, bytes: []const u8, diagnostic: *abi.Diagnostic) (Error || std.mem.Allocator.Error)!Image {
    const actual = checks.verdict(checker, bytes, bank);
    if (actual != expect) {
        diagnostic.* = .{ .path = name, .invariant = try std.fmt.allocPrint(arena, "states {t}, the reference gives {t}", .{ expect, actual }) };
        return error.WrongVerdict;
    }
    return .{ .name = name, .checker = checker.name, .bank = bank, .expect = expect, .canonical = canonical(checker, bytes), .bytes = bytes };
}

/// One edit with its field path resolved to an offset.
const Patch = union(enum) {
    set: Scalar,
    add: Scalar,
    byte: struct { offset: usize, value: u8 },
    resize: i8,

    const Scalar = struct { offset: usize, width: usize, value: u64 };
};

/// A described vector decoded against the model; executing it cannot fail on a path.
pub const Plan = struct {
    vector: Vector,
    checker: checks.Checker,
    patches: []const Patch,
};

/// Decodes a description. Nothing is mutated until `execute`.
pub fn resolve(arena: std.mem.Allocator, vector: Vector) (Error || std.mem.Allocator.Error)!Plan {
    try validate(vector);
    const checker = checks.findChecker(&checks.checkers, vector.checker).?;
    const patches = try arena.alloc(Patch, vector.edits.len);
    for (vector.edits, patches) |edit, *patch| patch.* = switch (edit) {
        .set => |set| blk: {
            const location = try scalar(checker.record, set.field);
            break :blk .{ .set = .{ .offset = location.offset, .width = location.width, .value = set.value } };
        },
        .add => |set| blk: {
            const location = try scalar(checker.record, set.field);
            break :blk .{ .add = .{ .offset = location.offset, .width = location.width, .value = set.value } };
        },
        .byte => |byte| .{ .byte = .{ .offset = (try locate(checker.record, byte.field)).offset + byte.index, .value = byte.value } },
        .resize => |delta| .{ .resize = delta },
    };
    return .{ .vector = vector, .checker = checker, .patches = patches };
}

/// Applies a plan to a fresh base image. The bytes come from `arena`.
pub fn execute(arena: std.mem.Allocator, plan: Plan) (Error || std.mem.Allocator.Error)![]u8 {
    var bytes = try baseImage(arena, plan.vector, plan.checker);
    var len = bytes.len;
    for (plan.patches) |patch| switch (patch) {
        .set => |set| store(bytes, set.offset, set.width, set.value),
        .add => |add| store(bytes, add.offset, add.width, load(bytes, add.offset, add.width) +% add.value),
        .byte => |byte| bytes[byte.offset] = byte.value,
        .resize => |delta| len = if (delta > 0) len + 1 else len - 1,
    };
    if (plan.vector.reseal) resealBanks(bytes);
    if (len != bytes.len) {
        const old = bytes.len;
        bytes = try arena.realloc(bytes, len);
        if (len > old) @memset(bytes[old..], 0);
    }
    return bytes;
}

/// Builds one described vector. Every allocation, including the returned bytes, comes from
/// `arena` and lives until it is reset.
pub fn build(arena: std.mem.Allocator, vector: Vector, diagnostic: *abi.Diagnostic) (Error || std.mem.Allocator.Error)!Image {
    diagnostic.* = .{ .path = vector.name, .invariant = "described vector is valid" };
    const plan = try resolve(arena, vector);
    const bytes = try execute(arena, plan);
    return finish(arena, plan.checker, vector.name, vector.bank, vector.expect, bytes, diagnostic);
}

/// A snapshot of every publication trace, before its first step and after each one.
fn traced(arena: std.mem.Allocator, list: *std.ArrayList(Image), diagnostic: *abi.Diagnostic) (Error || std.mem.Allocator.Error)!void {
    for (publication.traces) |trace| {
        diagnostic.* = .{ .path = trace.name, .invariant = "trace names a checker with a sample" };
        const checker = checks.findChecker(&checks.checkers, trace.checker) orelse return error.UnknownChecker;
        const page = publication.startPage(arena, trace) catch |err| switch (err) {
            error.OutOfMemory => return error.OutOfMemory,
            error.NoSample => return error.UnknownChecker,
        };
        var writer: publication.Writer = .init(trace, page);
        var label: []const u8 = "start";
        while (true) {
            const name = try std.fmt.allocPrint(arena, "trace_{s}_{d}_{s}", .{ trace.name, writer.next, label });
            try list.append(arena, try finish(arena, checker, name, trace.bank, writer.expected(), try arena.dupe(u8, page), diagnostic));
            const step = writer.step() catch |err| switch (err) {
                error.Finished => break,
                error.StepNotInProtocol => return error.StepNotInProtocol,
            };
            label = @tagName(step);
        }
    }
}

/// Absolute bases of the first and last instance a rule reads in `bytes`, or null when none is
/// live. Selected rules use bank 0.
fn instances(scope: checks.Scope, bytes: []const u8) ?[2]usize {
    return switch (scope) {
        .whole => .{ 0, 0 },
        .selected => |rows| .{ rows.base, rows.base },
        .rows => |rows| .{ rows.base, rows.base + (rows.count - 1) * rows.stride },
        .window => |window| blk: {
            const head = load(bytes, window.head_offset, 8);
            const live = @min(head, window.capacity);
            if (live == 0) break :blk null;
            const first: usize = @intCast((head - live) % window.capacity);
            const last: usize = @intCast((head - 1) % window.capacity);
            break :blk .{ window.base + first * window.stride, window.base + last * window.stride };
        },
    };
}

fn declared(values: []const model.EnumValue, value: u64) bool {
    for (values) |item| {
        if (item.value == value) return true;
    }
    return false;
}

fn pushUnique(arena: std.mem.Allocator, list: *std.ArrayList(u64), value: u64) std.mem.Allocator.Error!void {
    if (std.mem.indexOfScalar(u64, list.items, value) == null) try list.append(arena, value);
}

/// Undeclared values of an enum rule: zero where rejected or undeclared, one below the first
/// declared value, the first value of every gap, and one past the last.
fn undeclared(arena: std.mem.Allocator, member: checks.Member) std.mem.Allocator.Error![]const u64 {
    const values = checks.findEnum(model.description, member.enumeration).?.values;
    var lowest: u64 = std.math.maxInt(u64);
    var highest: u64 = 0;
    for (values) |item| {
        lowest = @min(lowest, item.value);
        highest = @max(highest, item.value);
    }
    var list: std.ArrayList(u64) = .empty;
    if (member.reject_zero or !declared(values, 0)) try pushUnique(arena, &list, 0);
    if (lowest > 1 and !declared(values, lowest - 1)) try pushUnique(arena, &list, lowest - 1);
    for (values) |item| {
        if (item.value < highest and !declared(values, item.value + 1)) try pushUnique(arena, &list, item.value + 1);
    }
    const limit = if (member.int.width >= 8) std.math.maxInt(u64) else (@as(u64, 1) << @intCast(member.int.width * 8)) - 1;
    if (highest < limit) try pushUnique(arena, &list, highest + 1);
    return list.items;
}

/// Sweeps derived from the rule tables; each expects its rule's own class.
fn sweeps(arena: std.mem.Allocator, list: *std.ArrayList(Image), diagnostic: *abi.Diagnostic) (Error || std.mem.Allocator.Error)!void {
    for (checks.checkers) |checker| {
        const valid = try sample(arena, checker);
        for (checker.rules, 0..) |rule, index| switch (rule.check) {
            .zero => |span| {
                const bases = instances(rule.scope, valid) orelse continue;
                const ends = [_]usize{ bases[0] + span.offset, bases[1] + span.offset + span.len - 1 };
                for (ends, [_][]const u8{ "first", "last" }) |at, which| {
                    const bytes = try arena.dupe(u8, valid);
                    bytes[at] = 1;
                    // A spec bank's CRC covers its reserved tail; reseal so reserved is reached.
                    if (std.mem.eql(u8, checker.record, "SpecPage")) resealBanks(bytes);
                    const name = try std.fmt.allocPrint(arena, "sweep_{s}_r{d}_reserved_{s}", .{ checker.name, index, which });
                    try list.append(arena, try finish(arena, checker, name, 0, .reserved, bytes, diagnostic));
                }
            },
            .member => |member| {
                const base = (instances(rule.scope, valid) orelse continue)[0];
                for (try undeclared(arena, member)) |value| {
                    const bytes = try arena.dupe(u8, valid);
                    store(bytes, base + member.int.offset, member.int.width, value);
                    const name = try std.fmt.allocPrint(arena, "sweep_{s}_r{d}_value_{d}", .{ checker.name, index, value });
                    try list.append(arena, try finish(arena, checker, name, 0, .unknown_kind, bytes, diagnostic));
                }
            },
            // Described vectors cover the remaining rule kinds with chosen values.
            .magic, .version, .seqlock, .nonzero, .crc32, .equal, .at_most, .entry_identity, .payload_tail => {},
        };
        const short = try arena.dupe(u8, valid[0 .. valid.len - 1]);
        try list.append(arena, try finish(arena, checker, try std.fmt.allocPrint(arena, "sweep_{s}_size_short", .{checker.name}), 0, .size, short, diagnostic));
        const long = try arena.alloc(u8, valid.len + 1);
        @memcpy(long[0..valid.len], valid);
        long[valid.len] = 0;
        try list.append(arena, try finish(arena, checker, try std.fmt.allocPrint(arena, "sweep_{s}_size_long", .{checker.name}), 0, .size, long, diagnostic));
    }
}

/// Every vector: described, traced, then swept. Everything comes from `arena`.
pub fn all(arena: std.mem.Allocator, diagnostic: *abi.Diagnostic) (Error || std.mem.Allocator.Error)![]const Image {
    var list: std.ArrayList(Image) = .empty;
    for (authored) |vector| try list.append(arena, try build(arena, vector, diagnostic));
    try traced(arena, &list, diagnostic);
    try sweeps(arena, &list, diagnostic);
    for (list.items, 0..) |image, i| {
        diagnostic.* = .{ .path = image.name, .invariant = "unique lower-case vector name" };
        if (!validName(image.name)) return error.BadName;
        for (list.items[0..i]) |prior| {
            if (std.mem.eql(u8, prior.name, image.name)) return error.DuplicateName;
        }
    }
    diagnostic.* = .{};
    return list.items;
}

pub const manifest_version = 1;

/// `chryso-abi-vectors <version> <count> <digest>`, then `name checker bank class canonical size`.
pub fn renderManifest(allocator: std.mem.Allocator, images: []const Image) (Writer.Error || std.mem.Allocator.Error)![]u8 {
    var out: Writer.Allocating = .init(allocator);
    defer out.deinit();
    const w = &out.writer;
    try w.print("chryso-abi-vectors {d} {d} {x:0>8}\n", .{ manifest_version, images.len, model.layoutDigest(model.description) });
    for (images) |image| {
        try w.print("{s} {s} {d} {t} {d} {d}\n", .{ image.name, image.checker, image.bank, image.expect, @intFromBool(image.canonical), image.bytes.len });
    }
    return out.toOwnedSlice();
}

pub fn writeAll(allocator: std.mem.Allocator, dir: std.fs.Dir, diagnostic: *abi.Diagnostic) !void {
    var arena: std.heap.ArenaAllocator = .init(allocator);
    defer arena.deinit();
    const images = try all(arena.allocator(), diagnostic);
    for (images) |image| {
        const path = try std.fmt.allocPrint(arena.allocator(), "{s}.bin", .{image.name});
        try dir.writeFile(.{ .sub_path = path, .data = image.bytes });
    }
    try dir.writeFile(.{ .sub_path = "manifest.txt", .data = try renderManifest(arena.allocator(), images) });
}

test "paths resolve against the model" {
    try std.testing.expectEqual(Location{ .offset = 16, .width = 8, .len = 8 }, try locate("RootStatusPage", "header.seq"));
    const row = try locate("RootStatusPage", "children[3].reserved");
    try std.testing.expectEqual(comptime checks.at(abi.RootStatusPage, "children") + 3 * @sizeOf(abi.RootChildStatus) + checks.at(abi.RootChildStatus, "reserved"), row.offset);
    try std.testing.expectEqual(Location{ .offset = 64 + 2016 + 24 + 12, .width = 4, .len = 4 }, try locate("SpecPage", "banks[1].budget[3]"));
    try std.testing.expectEqual(@as(usize, 192), (try locate("JournalPage", "entries[0].payload")).len);
    try std.testing.expectError(error.IndexOutOfRange, locate("SpecPage", "banks[2].generation"));
    try std.testing.expectError(error.UnknownField, locate("SpecPage", "banks.generation"));
    try std.testing.expectError(error.UnknownField, locate("SpecPage", "header"));
    try std.testing.expectError(error.UnknownField, locate("SpecPage", "header.nope"));
    try std.testing.expectError(error.NotScalar, scalar("SpecPage", "banks[0].budget"));
}

test "descriptions are rejected before any bytes are built" {
    const bad = [_]Vector{
        .{ .name = "Upper", .checker = "ctl_command", .expect = .ok },
        .{ .name = "x", .checker = "nope", .expect = .ok },
        .{ .name = "x", .checker = "ctl_command", .base = .journal_max, .expect = .ok },
        .{ .name = "x", .checker = "ctl_command", .reseal = true, .expect = .ok },
        .{ .name = "x", .checker = "ctl_command", .edits = &.{.{ .set = .{ .field = "version", .value = 0x10000 } }}, .expect = .ok },
        .{ .name = "x", .checker = "ctl_command", .edits = &.{.{ .byte = .{ .field = "magic", .index = 8, .value = 0 } }}, .expect = .ok },
        .{ .name = "x", .checker = "ctl_command", .base = .hex, .hex = "0", .expect = .ok },
    };
    for (bad) |vector| {
        if (validate(vector)) |_| {
            std.debug.print("accepted {any}\n", .{vector});
            return error.AcceptedBadVector;
        } else |_| {}
    }
}

test "a stated verdict the reference disagrees with is refused" {
    var arena: std.heap.ArenaAllocator = .init(std.testing.allocator);
    defer arena.deinit();
    var diagnostic: abi.Diagnostic = .{};
    const wrong: Vector = .{ .name = "wrong", .checker = "ctl_command", .edits = &.{.{ .set = .{ .field = "version", .value = 2 } }}, .expect = .ok };
    try std.testing.expectError(error.WrongVerdict, build(arena.allocator(), wrong, &diagnostic));
    try std.testing.expectEqualStrings("wrong", diagnostic.path);
    try std.testing.expectEqualStrings("states ok, the reference gives version", diagnostic.invariant);
}

test "every checker has a valid vector, a size vector and one per class its rules use" {
    var arena: std.heap.ArenaAllocator = .init(std.testing.allocator);
    defer arena.deinit();
    var diagnostic: abi.Diagnostic = .{};
    const images = try all(arena.allocator(), &diagnostic);
    try std.testing.expect(images.len > authored.len);
    var anywhere: [std.meta.fields(Class).len]bool = @splat(false);
    for (checks.checkers) |checker| {
        var seen: [std.meta.fields(Class).len]bool = @splat(false);
        for (images) |image| {
            if (!std.mem.eql(u8, image.checker, checker.name)) continue;
            seen[@intFromEnum(image.expect)] = true;
            anywhere[@intFromEnum(image.expect)] = true;
        }
        try std.testing.expect(seen[@intFromEnum(Class.ok)]);
        try std.testing.expect(seen[@intFromEnum(Class.size)]);
        for (checker.rules) |rule| {
            if (!seen[@intFromEnum(rule.class)]) {
                std.debug.print("{s}: no {t} vector\n", .{ checker.name, rule.class });
                return error.MissingClass;
            }
        }
    }
    for (anywhere) |seen| try std.testing.expect(seen);
}

test "generation is deterministic" {
    var arena: std.heap.ArenaAllocator = .init(std.testing.allocator);
    defer arena.deinit();
    var diagnostic: abi.Diagnostic = .{};
    const first = try all(arena.allocator(), &diagnostic);
    const second = try all(arena.allocator(), &diagnostic);
    try std.testing.expectEqualStrings(try renderManifest(arena.allocator(), first), try renderManifest(arena.allocator(), second));
    for (first, second) |a, b| try std.testing.expectEqualSlices(u8, a.bytes, b.bytes);
}

test "pinned images equal the model's samples" {
    var arena: std.heap.ArenaAllocator = .init(std.testing.allocator);
    defer arena.deinit();
    var diagnostic: abi.Diagnostic = .{};
    var pinned: usize = 0;
    for (authored) |vector| {
        if (vector.base != .hex) continue;
        pinned += 1;
        const image = try build(arena.allocator(), vector, &diagnostic);
        const checker = checks.findChecker(&checks.checkers, vector.checker).?;
        try std.testing.expectEqualSlices(u8, try sample(arena.allocator(), checker), image.bytes);
    }
    try std.testing.expect(pinned >= 2);
    // The pinned bank's CRC was computed outside the project; the model must seal the same value.
    var bank = samples.spec().banks[0];
    samples.sealBank(&bank);
    try std.testing.expectEqual(@as(u32, 0x19b0a44c), bank.crc32);
    try std.testing.expectEqual(@as(u32, 0xCBF43926), std.hash.Crc32.hash("123456789"));
}

test "the manifest header carries the layout digest and the count" {
    var arena: std.heap.ArenaAllocator = .init(std.testing.allocator);
    defer arena.deinit();
    var diagnostic: abi.Diagnostic = .{};
    const images = try all(arena.allocator(), &diagnostic);
    const text = try renderManifest(arena.allocator(), images);
    const header = try std.fmt.allocPrint(arena.allocator(), "chryso-abi-vectors 1 {d} {x:0>8}\n", .{ images.len, model.layoutDigest(model.description) });
    try std.testing.expect(std.mem.startsWith(u8, text, header));
    try std.testing.expectEqual(images.len + 1, std.mem.count(u8, text, "\n"));
}
