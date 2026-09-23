//! Host-only projection of the typed system ABI for pure Nix evaluation.
const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const serde = b.dependency("serde", .{ .target = target, .optimize = optimize });
    const module = b.createModule(.{
        .root_source_file = b.path("main.zig"),
        .target = target,
        .optimize = optimize,
    });
    module.addImport("serde", serde.module("serde"));
    module.addImport("system_abi", b.createModule(.{
        .root_source_file = b.path("../../interfaces/system_abi.zig"),
        .target = target,
        .optimize = optimize,
    }));
    const exe = b.addExecutable(.{ .name = "gen-system-abi", .root_module = module });
    b.installArtifact(exe);
}
