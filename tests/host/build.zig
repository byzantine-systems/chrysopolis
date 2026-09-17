const std = @import("std");

// Host test harness for the pure runtime units in src/runtime.
//
// A separate package rather than a step in the root build.zig: the root build
// requires every cross -D path (board dir, sDDF, LionsOS, ...) before it can
// configure anything, and these suites need none of them. tools/sdf is split
// out for the same reason.
//
// Each suite is one C executable that links only pure units, which include no
// Microkit, seL4, sDDF, LionsOS or libmicrokitco header. Nothing here adds
// those include paths, so a pure unit that grows such an include stops
// compiling in this harness.
//
// Every suite runs twice: Debug with UBSan in trap mode, and ReleaseFast so
// optimizer-sensitive undefined behaviour gets a second chance to show up.

// The root build.zig's first-party C flags plus its diagnostic warnings, minus
// -ffreestanding (the suites are hosted programs). Keep the two in sync.
const cflags = [_][]const u8{
    "-std=c23",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-prototypes",
    "-Wmissing-variable-declarations",
    "-D_POSIX_C_SOURCE=200809L",
};

const Suite = struct {
    name: []const u8,
    // Pure unit sources from src/runtime linked into the suite. Header-only
    // units need no entry.
    units: []const []const u8,
};

const suites = [_]Suite{
    .{ .name = "deadline", .units = &.{} },
    .{ .name = "stack", .units = &.{"runtime_stack.c"} },
    .{ .name = "timer_slot", .units = &.{} },
    .{ .name = "fd_pair", .units = &.{"runtime_fd_pair.c"} },
    .{ .name = "epoll", .units = &.{"runtime_epoll_table.c"} },
    .{ .name = "status", .units = &.{"runtime_status.c"} },
    .{ .name = "pd_restart", .units = &.{"runtime_pd_restart_parse.c"} },
    .{ .name = "pthread", .units = &.{"runtime_tls_row.c"} },
    .{ .name = "root_policy", .units = &.{} },
    .{ .name = "snapshot", .units = &.{"beam_snapshot_codec.c"} },
    .{ .name = "rng_select", .units = &.{"rng_select.c"} },
    .{ .name = "tcp_logic", .units = &.{} },
};

const Variant = struct {
    suffix: []const u8,
    optimize: std.builtin.OptimizeMode,
    sanitize_c: std.zig.SanitizeC,
};

const variants = [_]Variant{
    .{ .suffix = "debug", .optimize = .Debug, .sanitize_c = .trap },
    .{ .suffix = "release", .optimize = .ReleaseFast, .sanitize_c = .off },
};

pub fn build(b: *std.Build) void {
    // On Linux the suites link statically against Zig's bundled musl. The Nix
    // sandbox has no system dynamic linker for Zig to detect, and a static
    // executable needs none.
    const linux = @import("builtin").os.tag == .linux;
    const target = b.standardTargetOptions(.{
        .default_target = if (linux) .{ .abi = .musl } else .{},
    });
    const test_step = b.step("test", "Run every runtime host suite in every variant");
    const runtime_dir = b.path("../../src/runtime");

    for (variants) |variant| {
        for (suites) |suite| {
            const exe = b.addExecutable(.{
                .name = b.fmt("suite_{s}_{s}", .{ suite.name, variant.suffix }),
                .linkage = if (linux) .static else null,
                .root_module = b.createModule(.{
                    .target = target,
                    .optimize = variant.optimize,
                    .link_libc = true,
                    .sanitize_c = variant.sanitize_c,
                }),
            });
            exe.root_module.addCSourceFile(.{
                .file = b.path(b.fmt("suite_{s}.c", .{suite.name})),
                .flags = &cflags,
            });
            for (suite.units) |unit| {
                exe.root_module.addCSourceFile(.{
                    .file = runtime_dir.path(b, unit),
                    .flags = &cflags,
                });
            }
            exe.root_module.addIncludePath(runtime_dir);

            const run = b.addRunArtifact(exe);
            run.expectExitCode(0);
            test_step.dependOn(&run.step);
        }
    }
}
