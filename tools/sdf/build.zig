const std = @import("std");

// Builds the gen-sdf tool. The sdfgen dependency is resolved through the
// Zig package manager (build.zig.zon), with zig2nix supplying the fetched
// packages offline. We consume sdfgen's "sdf" module, that is mod.zig, the
// full API (SystemDescription + the sddf/lionsos/dtb helpers), rather than
// importing src/sdf.zig directly, because the reference-stack topology needs
// the Sddf.Serial/Timer subsystem helpers and DTB parsing. sdfgen's own
// build.zig wires the transitive dtb.zig import into that module for us.
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const sdfgen = b.dependency("sdfgen", .{});

    const exe_mod = b.createModule(.{
        .root_source_file = b.path("system.zig"),
        .target = target,
        .optimize = optimize,
    });
    exe_mod.addImport("sdf", sdfgen.module("sdf"));

    const exe = b.addExecutable(.{
        .name = "gen-sdf",
        .root_module = exe_mod,
    });
    b.installArtifact(exe);
}
