const std = @import("std");
const microkit = @import("microkit.zig");

pub const BeamCfg = struct {
    glue_obj: std.Build.LazyPath,
    microkitco_obj: std.Build.LazyPath,
    lwip_obj: std.Build.LazyPath,
    tcp_obj: std.Build.LazyPath,
    bearssl_obj: std.Build.LazyPath,
    board_dir: []const u8,
    lions_libc: []const u8,
    libc_dir: []const u8,
    erts_dir: ?[]const u8,
    lazy: std.Build.Module.LinkSystemLibraryOptions,
};

pub fn addBeamExe(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    cfg: BeamCfg,
    name: []const u8,
    with_erts: bool,
    util_putchar_debug: *std.Build.Step.Compile,
) void {
    const exe = b.addExecutable(.{
        .name = name,
        .root_module = b.createModule(.{ .target = target, .optimize = .ReleaseFast, .strip = false }),
    });
    exe.root_module.addObjectFile(cfg.glue_obj);
    exe.root_module.addObjectFile(cfg.microkitco_obj);
    exe.root_module.addObjectFile(cfg.lwip_obj);
    exe.root_module.addObjectFile(cfg.tcp_obj);
    exe.root_module.addObjectFile(cfg.bearssl_obj);
    exe.root_module.linkLibrary(util_putchar_debug);
    exe.root_module.addLibraryPath(.{ .cwd_relative = b.fmt("{s}/lib", .{cfg.board_dir}) });
    exe.root_module.addLibraryPath(.{ .cwd_relative = b.fmt("{s}/lib", .{cfg.lions_libc}) });
    exe.root_module.addLibraryPath(.{ .cwd_relative = cfg.libc_dir });
    if (with_erts) {
        exe.root_module.addLibraryPath(.{ .cwd_relative = cfg.erts_dir.? });
        exe.root_module.linkSystemLibrary("erts_all", cfg.lazy);
    }
    exe.root_module.linkSystemLibrary("microkit", cfg.lazy);
    exe.root_module.linkSystemLibrary("lionsc", cfg.lazy);
    exe.setLinkerScript(.{ .cwd_relative = b.fmt("{s}/lib/microkit.ld", .{cfg.board_dir}) });
    exe.pie = false;
    exe.bundle_compiler_rt = false;
    exe.link_gc_sections = false;
    if (with_erts) exe.forceUndefinedSymbol("erl_start");
    b.installArtifact(exe);
}

pub fn addRoot(
    ctx: microkit.Context,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    source: std.Build.LazyPath,
    flags: []const []const u8,
    generated_abi: *std.Build.Step.ConfigHeader,
) *std.Build.Step.Compile {
    const root_pd = microkit.addPd(ctx, "root.elf", target, optimize);
    root_pd.root_module.addCSourceFile(.{ .file = source, .flags = flags });
    root_pd.root_module.addConfigHeader(generated_abi);
    root_pd.link_gc_sections = false;
    ctx.b.installArtifact(root_pd);
    return root_pd;
}
