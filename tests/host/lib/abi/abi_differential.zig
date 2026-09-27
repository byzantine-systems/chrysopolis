//! The generated C checkers agree with the Zig reference on seeded mutations of valid samples.
const std = @import("std");
const abi = @import("orchestrator_abi");
const checks = @import("checks");

extern fn abi_shim_check(name: [*]const u8, name_len: usize, bytes: [*]const u8, len: usize, bank: usize) u8;

const rounds = 4000;

fn cVerdict(name: []const u8, bytes: []const u8, bank: usize) error{UnknownCheckerInShim}!checks.Class {
    const raw = abi_shim_check(name.ptr, name.len, bytes.ptr, bytes.len, bank);
    return std.meta.intToEnum(checks.Class, raw) catch error.UnknownCheckerInShim;
}

/// A byte span a rule reads, so mutations land where verdicts can change.
fn ruleSpan(check: checks.Check, random: std.Random) checks.Span {
    return switch (check) {
        .magic, .version, .equal, .at_most => |value| .{ .offset = value.int.offset, .len = value.int.width },
        .member => |value| .{ .offset = value.int.offset, .len = value.int.width },
        .seqlock, .nonzero => |value| .{ .offset = value.offset, .len = value.width },
        .zero => |value| value,
        .crc32 => |value| .{ .offset = value.crc.offset, .len = value.crc.width },
        .entry_identity => |value| if (random.boolean())
            .{ .offset = value.sequence.offset, .len = 8 }
        else
            .{ .offset = value.generation.offset, .len = 8 },
        .payload_tail => |value| if (random.boolean())
            .{ .offset = value.length.offset, .len = value.length.width }
        else
            value.payload,
    };
}

fn targetOffset(checker: checks.Checker, random: std.Random) usize {
    if (random.uintLessThan(u8, 4) == 0) return random.uintLessThan(usize, checker.size);
    const rule = checker.rules[random.uintLessThan(usize, checker.rules.len)];
    const base: usize = switch (rule.scope) {
        .whole => 0,
        .rows, .selected => |rows| rows.base + random.uintLessThan(usize, rows.count) * rows.stride,
        .window => |window| window.base + random.uintLessThan(usize, window.capacity) * window.stride,
    };
    const span = ruleSpan(rule.check, random);
    // Window heads decide which rows are live; hit them too.
    if (rule.scope == .window and random.boolean()) return rule.scope.window.head_offset + random.uintLessThan(usize, 8);
    return base + span.offset + random.uintLessThan(usize, span.len);
}

fn mutationByte(random: std.Random) u8 {
    const interesting = [_]u8{ 0, 1, 2, 3, 0x3e, 0x80, 0xc0, 0xc1, 0xff };
    return if (random.boolean()) interesting[random.uintLessThan(usize, interesting.len)] else random.int(u8);
}

fn resealBanks(bytes: []u8) void {
    const base = comptime checks.at(abi.SpecPage, "banks");
    for (0..2) |i| {
        const at = base + i * @sizeOf(abi.SpecBank);
        var bank = std.mem.bytesToValue(abi.SpecBank, bytes[at..][0..@sizeOf(abi.SpecBank)]);
        checks.samples.sealBank(&bank);
        @memcpy(bytes[at..][0..@sizeOf(abi.SpecBank)], std.mem.asBytes(&bank));
    }
}

test "C checkers match the Zig reference" {
    const allocator = std.testing.allocator;
    // Fixed seed: a failure reproduces exactly.
    var prng: std.Random.DefaultPrng = .init(0x0c4a_b1e5);
    const random = prng.random();
    for (checks.checkers) |checker| {
        const sample = try checks.samples.image(allocator, checker.name);
        defer allocator.free(sample);
        // One spare byte so half the calls pass an odd address to C.
        const storage = try allocator.alloc(u8, sample.len + 1);
        defer allocator.free(storage);
        var seen: [std.meta.fields(checks.Class).len]usize = @splat(0);
        for (0..rounds) |round| {
            const bytes = storage[round % 2 ..][0..sample.len];
            @memcpy(bytes, sample);
            for (0..random.intRangeAtMost(usize, 1, 3)) |_| bytes[targetOffset(checker, random)] = mutationByte(random);
            if (checker.selector != null and random.boolean()) resealBanks(bytes);
            const bank = random.uintLessThan(usize, 3);
            const expected = checks.verdict(checker, bytes, bank);
            const actual = try cVerdict(checker.name, bytes, bank);
            if (expected != actual) {
                std.debug.print("{s} round {d}: zig {t}, C {t}\n", .{ checker.name, round, expected, actual });
                return error.CheckerMismatch;
            }
            seen[@intFromEnum(expected)] += 1;
        }
        // The mutations must reach both acceptance and rejection.
        try std.testing.expect(seen[@intFromEnum(checks.Class.ok)] > 0);
        try std.testing.expect(seen[@intFromEnum(checks.Class.ok)] < rounds);
        const short = try cVerdict(checker.name, sample[0 .. sample.len - 1], 0);
        try std.testing.expectEqual(checks.Class.size, short);
    }
}

test "the shim knows every checker" {
    const bytes = [_]u8{0};
    for (checks.checkers) |checker| _ = try cVerdict(checker.name, &bytes, 0);
    try std.testing.expectError(error.UnknownCheckerInShim, cVerdict("missing", &bytes, 0));
}
