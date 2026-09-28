//! Typed ABI tools. Standalone it tests the contracts and builds both generators; as a path
//! dependency it gives other builds the generated C headers (`orchestrator-abi`), the golden
//! vectors (`orchestrator-abi-vectors`) and the `orchestrator_abi` and `checks` modules.
//! serde is lazy, so dependents never fetch it.
const std = @import("std");

pub const headers = @import("headers.zig").names;

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const module = struct {
        fn make(owner: *std.Build, path: []const u8, t: std.Build.ResolvedTarget, o: std.builtin.OptimizeMode) *std.Build.Module {
            return owner.createModule(.{ .root_source_file = owner.path(path), .target = t, .optimize = o });
        }
    }.make;
    const system_abi = module(b, "../../interfaces/system_abi.zig", target, optimize);
    const orchestrator_abi = b.addModule("orchestrator_abi", .{
        .root_source_file = b.path("../../interfaces/orchestrator_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const model = module(b, "model.zig", target, optimize);
    model.addImport("orchestrator_abi", orchestrator_abi);
    const checks = b.addModule("checks", .{
        .root_source_file = b.path("checks.zig"),
        .target = target,
        .optimize = optimize,
    });
    checks.addImport("orchestrator_abi", orchestrator_abi);
    checks.addImport("model", model);
    const validation = module(b, "validate.zig", target, optimize);
    const naming = module(b, "naming.zig", target, optimize);
    const generate_c = module(b, "generate_c.zig", target, optimize);
    const generate_erlang = module(b, "generate_erlang.zig", target, optimize);
    const publication = module(b, "publication.zig", target, optimize);
    publication.addImport("orchestrator_abi", orchestrator_abi);
    publication.addImport("checks", checks);
    const generate_vectors = module(b, "generate_vectors.zig", target, optimize);
    generate_vectors.addImport("publication", publication);
    for ([_]*std.Build.Module{ validation, generate_c, generate_erlang, generate_vectors }) |consumer| {
        consumer.addImport("naming", naming);
        consumer.addImport("system_abi", system_abi);
        consumer.addImport("orchestrator_abi", orchestrator_abi);
        consumer.addImport("model", model);
        consumer.addImport("checks", checks);
    }

    const generator = b.addExecutable(.{
        .name = "gen-orchestrator-abi",
        .root_module = b.createModule(.{
            .root_source_file = b.path("gen_orchestrator_abi.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "system_abi", .module = system_abi },
                .{ .name = "orchestrator_abi", .module = orchestrator_abi },
                .{ .name = "model", .module = model },
                .{ .name = "validation", .module = validation },
                .{ .name = "generate_c", .module = generate_c },
                .{ .name = "generate_erlang", .module = generate_erlang },
                .{ .name = "generate_vectors", .module = generate_vectors },
            },
        }),
    });
    // Dependents pass a musl target: the Nix sandbox has no dynamic linker to detect.
    if (target.result.abi == .musl) generator.linkage = .static;
    b.installArtifact(generator);
    const generate = b.addRunArtifact(generator);
    generate.addArg("c");
    b.addNamedLazyPath("orchestrator-abi", generate.addOutputDirectoryArg("orchestrator-abi"));
    const generate_erl = b.addRunArtifact(generator);
    generate_erl.addArg("erlang");
    b.addNamedLazyPath("orchestrator-abi-erlang", generate_erl.addOutputDirectoryArg("orchestrator-abi-erlang"));
    const generate_vec = b.addRunArtifact(generator);
    generate_vec.addArg("vectors");
    b.addNamedLazyPath("orchestrator-abi-vectors", generate_vec.addOutputDirectoryArg("orchestrator-abi-vectors"));

    // The JSON projection needs serde. Ask for it only as the root build: a lazyDependency
    // call from a dependent's configure still fetches, and the Nix sandbox is offline.
    const serde = if (b.dep_prefix.len == 0) b.lazyDependency("serde", .{ .target = target, .optimize = optimize }) else null;
    if (serde) |dep| {
        const gen_abi = b.addExecutable(.{
            .name = "gen-abi",
            .root_module = b.createModule(.{
                .root_source_file = b.path("main.zig"),
                .target = target,
                .optimize = optimize,
                .imports = &.{
                    .{ .name = "serde", .module = dep.module("serde") },
                    .{ .name = "system_abi", .module = system_abi },
                    .{ .name = "orchestrator_abi", .module = orchestrator_abi },
                    .{ .name = "model", .module = model },
                    .{ .name = "validation", .module = validation },
                },
            }),
        });
        b.installArtifact(gen_abi);
    }

    const test_step = b.step("test", "Test both typed ABI contracts and their projection");
    for ([_]*std.Build.Module{ system_abi, orchestrator_abi, model, checks, validation, naming, publication, generate_c, generate_erlang, generate_vectors }) |test_module| {
        const unit_tests = b.addTest(.{ .root_module = test_module });
        test_step.dependOn(&b.addRunArtifact(unit_tests).step);
    }
}
