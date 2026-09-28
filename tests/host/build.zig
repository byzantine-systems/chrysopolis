const std = @import("std");

// Host test harness for the pure BEAM units in src/pd/beam and Root policy in
// src/pd/root/policy.
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
    source: []const u8,
    unit_dir: []const u8,
    extra_include_dir: ?[]const u8 = null,
    // Adds the generated <chrysopolis/...> orchestration headers.
    generated_abi: bool = false,
    extra_flags: []const []const u8 = &.{},
    // Harness sources beside the suite, built with the suite's flags.
    sources: []const []const u8 = &.{},
    // Passes the golden vector directory from tools/abi as argv[1].
    vectors: bool = false,
    // Pure unit sources from the owning directory. Header-only units need no entry.
    units: []const []const u8,
};

const suites = [_]Suite{
    .{ .name = "deadline", .source = "beam/compat/time/suite_deadline.c", .unit_dir = "../../src/pd/beam/compat/time", .units = &.{} },
    .{ .name = "stack", .source = "beam/compat/pthread/suite_stack.c", .unit_dir = "../../src/pd/beam/compat/pthread", .units = &.{"runtime_stack.c"} },
    .{ .name = "timer_slot", .source = "beam/io/timer/suite_timer_slot.c", .unit_dir = "../../src/pd/beam/io/timer", .units = &.{} },
    .{ .name = "fd_pair", .source = "beam/compat/fd/suite_fd_pair.c", .unit_dir = "../../src/pd/beam/compat/fd", .units = &.{"runtime_fd_pair.c"} },
    .{ .name = "epoll", .source = "beam/compat/poll/suite_epoll.c", .unit_dir = "../../src/pd/beam/compat/poll", .units = &.{"runtime_epoll_table.c"} },
    .{ .name = "status", .source = "beam/config/suite_status.c", .unit_dir = "../../src/pd/beam/config", .extra_include_dir = "../../src/pd/beam/compat/syscall", .units = &.{"runtime_status.c"} },
    .{ .name = "pd_restart", .source = "beam/restart/suite_pd_restart.c", .unit_dir = "../../src/pd/beam/restart", .units = &.{"runtime_pd_restart_parse.c"} },
    .{ .name = "pthread", .source = "beam/compat/pthread/suite_pthread.c", .unit_dir = "../../src/pd/beam/compat/pthread", .units = &.{"runtime_tls_row.c"} },
    .{ .name = "root_policy", .source = "root/suite_root_policy.c", .unit_dir = "../../src/pd/root/policy", .units = &.{} },
    .{ .name = "snapshot", .source = "beam/restart/suite_snapshot.c", .unit_dir = "../../src/pd/beam/restart", .units = &.{"beam_snapshot_codec.c"} },
    .{ .name = "restart_layout", .source = "beam/restart/suite_restart_layout.c", .unit_dir = "../../src/pd/beam/restart", .units = &.{} },
    .{ .name = "rng_select", .source = "beam/security/suite_rng_select.c", .unit_dir = "../../src/pd/beam/security", .units = &.{"rng_select.c"} },
    .{ .name = "tcp_logic", .source = "beam/io/network/suite_tcp_logic.c", .unit_dir = "../../src/pd/beam/io/network", .units = &.{} },
    .{ .name = "tcp_state", .source = "beam/io/network/suite_tcp_state.c", .unit_dir = "../../src/pd/beam/io/network", .units = &.{} },
    .{ .name = "abi_layout", .source = "lib/abi/suite_abi_layout.c", .unit_dir = "lib/abi", .generated_abi = true, .extra_flags = &abi_flags, .units = &.{} },
    .{ .name = "abi_vectors", .source = "lib/abi/suite_abi_vectors.c", .unit_dir = "lib/abi", .generated_abi = true, .extra_flags = &abi_flags, .sources = &.{"lib/abi/abi_shim.c"}, .vectors = true, .units = &.{} },
};

// Generated code must also be clean under the conversion warnings.
const abi_flags = [_][]const u8{ "-Wconversion", "-Wsign-conversion", "-Wshadow" };

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
    // tools/abi as a path dependency: generated headers plus the Zig reference checks.
    const abi = b.dependency("abi", .{ .target = target });
    const generated_abi = abi.namedLazyPath("orchestrator-abi");
    const golden_vectors = abi.namedLazyPath("orchestrator-abi-vectors");
    for (variants) |variant| {
        for (suites) |suite| {
            const owner_dir = b.path(suite.unit_dir);
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
            const suite_flags = std.mem.concat(b.allocator, []const u8, &.{ &cflags, suite.extra_flags }) catch @panic("OOM");
            exe.root_module.addCSourceFile(.{
                .file = b.path(suite.source),
                .flags = suite_flags,
            });
            for (suite.sources) |source| {
                exe.root_module.addCSourceFile(.{ .file = b.path(source), .flags = suite_flags });
            }
            for (suite.units) |unit| {
                exe.root_module.addCSourceFile(.{
                    .file = owner_dir.path(b, unit),
                    .flags = &cflags,
                });
            }
            exe.root_module.addIncludePath(b.path(".")); // check.h for suites under owner subdirectories
            exe.root_module.addIncludePath(owner_dir);
            if (suite.extra_include_dir) |dir| exe.root_module.addIncludePath(b.path(dir));
            if (suite.generated_abi) exe.root_module.addIncludePath(generated_abi);

            const run = b.addRunArtifact(exe);
            if (suite.vectors) run.addDirectoryArg(golden_vectors);
            run.expectExitCode(0);
            test_step.dependOn(&run.step);
        }

        // Generated C checkers against the Zig reference verdicts.
        const differential = b.addTest(.{
            .name = b.fmt("abi_differential_{s}", .{variant.suffix}),
            .root_module = b.createModule(.{
                .root_source_file = b.path("lib/abi/abi_differential.zig"),
                .target = target,
                .optimize = variant.optimize,
                .link_libc = true,
                .sanitize_c = variant.sanitize_c,
                .imports = &.{
                    .{ .name = "orchestrator_abi", .module = abi.module("orchestrator_abi") },
                    .{ .name = "checks", .module = abi.module("checks") },
                },
            }),
        });
        if (linux) differential.linkage = .static;
        const shim_flags = std.mem.concat(b.allocator, []const u8, &.{ &cflags, &abi_flags }) catch @panic("OOM");
        differential.root_module.addCSourceFile(.{ .file = b.path("lib/abi/abi_shim.c"), .flags = shim_flags });
        differential.root_module.addIncludePath(generated_abi);
        test_step.dependOn(&b.addRunArtifact(differential).step);
    }
}
