const std = @import("std");
const microkit = @import("microkit.zig");

pub const Libraries = struct {
    microkitco: *std.Build.Step.Compile,
    util: *std.Build.Step.Compile,
    util_putchar_debug: *std.Build.Step.Compile,
};

pub fn buildLibraries(
    ctx: microkit.Context,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    libmicrokitco_src: []const u8,
) Libraries {
    const b = ctx.b;
    const microkitco = b.addLibrary(.{
        .name = "microkitco",
        .linkage = .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize, .strip = false }),
    });
    microkitco.root_module.addCSourceFiles(.{
        .root = .{ .cwd_relative = libmicrokitco_src },
        .files = &.{ "libco/libco.c", "libmicrokitco.c" },
        .flags = &.{ "-ffreestanding", "-O2", "-g", "-Wall" },
    });
    microkitco.root_module.addIncludePath(.{ .cwd_relative = libmicrokitco_src });
    microkitco.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/libco", .{libmicrokitco_src}) });
    microkitco.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/libhostedqueue", .{libmicrokitco_src}) });
    microkitco.root_module.addIncludePath(ctx.libmicrokit_include);
    microkitco.root_module.addIncludePath(b.path("src/pd/beam/compat/pthread"));
    b.installArtifact(microkitco);

    const util = b.addLibrary(.{
        .name = "util",
        .linkage = .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize }),
    });
    util.root_module.addCSourceFiles(.{
        .root = .{ .cwd_relative = ctx.sddf },
        .files = &.{
            "util/cache.c",                      "util/fsmalloc.c",                    "util/bitarray.c",                   "util/assert.c",                     "util/custom_libc/libc.c",
            "util/custom_libc/aarch64/memcmp.S", "util/custom_libc/aarch64/memcpy.S",  "util/custom_libc/aarch64/memset.S", "util/custom_libc/aarch64/strcmp.S", "util/custom_libc/aarch64/strcpy.S",
            "util/custom_libc/aarch64/strlen.S", "util/custom_libc/aarch64/strncmp.S",
        },
    });
    microkit.addSddfIncludes(ctx, util.root_module);
    util.root_module.addIncludePath(ctx.libmicrokit_include);

    const util_putchar_debug = b.addLibrary(.{
        .name = "util_putchar_debug",
        .linkage = .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize }),
    });
    util_putchar_debug.root_module.addCSourceFiles(.{
        .root = .{ .cwd_relative = ctx.sddf },
        .files = &.{ "util/assert.c", "util/printf.c", "util/putchar_debug.c" },
    });
    microkit.addSddfIncludes(ctx, util_putchar_debug.root_module);
    util_putchar_debug.root_module.addIncludePath(ctx.libmicrokit_include);

    return .{ .microkitco = microkitco, .util = util, .util_putchar_debug = util_putchar_debug };
}
