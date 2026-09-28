//! Generated orchestration headers: the include path every PD compile gets, and their
//! cross-target diagnostic probes. Every build runs the generator, so a generator
//! regression fails the images, not only -Ddiagnostic.
const std = @import("std");

// Generated code must stay clean under the conversion warnings too.
const extra_warnings = [_][]const u8{ "-Wconversion", "-Wsign-conversion", "-Wshadow" };

/// The generated headers from the tools/abi path dependency, built for the build host.
fn generatedHeaders(b: *std.Build) std.Build.LazyPath {
    // Static musl on Linux: the Nix sandbox has no dynamic linker for native detection.
    const linux = @import("builtin").os.tag == .linux;
    const host = if (linux) b.resolveTargetQuery(.{ .abi = .musl }) else b.graph.host;
    return b.dependency("abi", .{ .target = host }).namedLazyPath("orchestrator-abi");
}

/// Makes `<chrysopolis/...>` resolvable from `module`.
pub fn addGenerated(b: *std.Build, module: *std.Build.Module) void {
    module.addIncludePath(generatedHeaders(b));
}

/// Compiles each header alone for the cross target, and links the atomic probe.
pub fn addProbes(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    flags: []const []const u8,
    runtime_abi: *std.Build.Step.ConfigHeader,
) void {
    const generated = generatedHeaders(b);
    const probe_flags = std.mem.concat(b.allocator, []const u8, &.{ flags, &extra_warnings }) catch @panic("OOM");

    const headers_probe = b.addObject(.{
        .name = "abi_header_probe",
        .root_module = b.createModule(.{ .target = target, .optimize = .ReleaseFast }),
    });
    for (@import("abi").headers) |header| {
        for ([_]bool{ false, true }) |with_runtime_abi| {
            const define = b.fmt("-DCHRYSO_ABI_HEADER=<chrysopolis/{s}>", .{header});
            const variant: []const []const u8 = if (with_runtime_abi) &.{ define, "-DCHRYSO_ABI_WITH_RUNTIME_ABI=1" } else &.{define};
            headers_probe.root_module.addCSourceFile(.{
                .file = b.path("src/lib/abi/abi_header_probe.c"),
                .flags = std.mem.concat(b.allocator, []const u8, &.{ probe_flags, variant }) catch @panic("OOM"),
            });
        }
    }
    headers_probe.root_module.addIncludePath(generated);
    headers_probe.root_module.addConfigHeader(runtime_abi);
    b.getInstallStep().dependOn(&headers_probe.step);

    // Production codegen settings: ReleaseFast, no sanitizer or compiler_rt helpers.
    const atomic_probe = b.addExecutable(.{
        .name = "abi_atomic_probe.elf",
        .root_module = b.createModule(.{ .target = target, .optimize = .ReleaseFast, .sanitize_c = .off, .strip = false }),
    });
    atomic_probe.root_module.addCSourceFile(.{ .file = b.path("src/lib/abi/abi_atomic_probe.c"), .flags = probe_flags });
    atomic_probe.root_module.addIncludePath(generated);
    atomic_probe.entry = .{ .symbol_name = "abi_atomic_probe_start" };
    atomic_probe.bundle_compiler_rt = false;
    atomic_probe.bundle_ubsan_rt = false;
    atomic_probe.pie = false;
    b.installArtifact(atomic_probe);
}
