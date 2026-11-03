const std = @import("std");

const first_party_cflags = [_][]const u8{
    "-std=c23",                  "-ffreestanding", "-O2", "-g", "-Wall", "-Wextra", "-Wpedantic",
    "-D_POSIX_C_SOURCE=200809L", "-Dasm=__asm__",
};
const diagnostic_cflags = first_party_cflags ++ [_][]const u8{"-Werror"};
const runtime_contract_diagnostic_cflags = diagnostic_cflags ++ [_][]const u8{
    "-Wmissing-prototypes", "-Wmissing-variable-declarations", "-DCHRYSO_DIAGNOSTIC=1",
};

pub fn firstParty(diagnostic: bool) []const []const u8 {
    return if (diagnostic) &diagnostic_cflags else &first_party_cflags;
}

pub fn runtime(diagnostic: bool) []const []const u8 {
    return if (diagnostic) &runtime_contract_diagnostic_cflags else &first_party_cflags;
}

/// `flags` plus one -D define, for a build option read at configure time.
pub fn withDefine(b: *std.Build, flags: []const []const u8, define: []const u8) []const []const u8 {
    const out = b.allocator.alloc([]const u8, flags.len + 1) catch @panic("OOM");
    @memcpy(out[0..flags.len], flags);
    out[flags.len] = define;
    return out;
}

pub fn tcp(diagnostic: bool, debug: bool) []const []const u8 {
    const base = [_][]const u8{
        "-ffreestanding",              "-O2",                       "-g",                   "-std=gnu23",                                      "-Wall", "-Wextra", "-Wsign-compare",
        "-Wno-bitwise-op-parentheses", "-Wno-shift-op-parentheses", "-Wno-unused-function", "-Wno-tautological-constant-out-of-range-compare",
    };
    return if (diagnostic and debug)
        &(base ++ [_][]const u8{ "-Werror", "-DTCP_DEBUG=1" })
    else if (diagnostic)
        &(base ++ [_][]const u8{"-Werror"})
    else if (debug)
        &(base ++ [_][]const u8{"-DTCP_DEBUG=1"})
    else
        &base;
}
