//! Host-only projection of the typed system and orchestration ABIs for pure Nix evaluation.
const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const serde = b.dependency("serde", .{ .target = target, .optimize = optimize });
    const system_abi = b.createModule(.{
        .root_source_file = b.path("../../interfaces/system_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const orchestrator_abi = b.createModule(.{
        .root_source_file = b.path("../../interfaces/orchestrator_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const model = b.createModule(.{
        .root_source_file = b.path("model.zig"),
        .target = target,
        .optimize = optimize,
    });
    model.addImport("orchestrator_abi", orchestrator_abi);
    const module = b.createModule(.{
        .root_source_file = b.path("main.zig"),
        .target = target,
        .optimize = optimize,
    });
    module.addImport("serde", serde.module("serde"));
    module.addImport("system_abi", system_abi);
    module.addImport("orchestrator_abi", orchestrator_abi);
    module.addImport("model", model);
    const exe = b.addExecutable(.{ .name = "gen-abi", .root_module = module });
    b.installArtifact(exe);

    const test_step = b.step("test", "Test both typed ABI contracts and their projection");
    inline for (.{ system_abi, orchestrator_abi, model }) |test_module| {
        const unit_tests = b.addTest(.{ .root_module = test_module });
        test_step.dependOn(&b.addRunArtifact(unit_tests).step);
    }
}
