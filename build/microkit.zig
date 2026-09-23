const std = @import("std");
const target = @import("target.zig");

pub const Context = struct {
    b: *std.Build,
    sddf: []const u8,
    libmicrokit: std.Build.LazyPath,
    libmicrokit_include: std.Build.LazyPath,
    libmicrokit_linker_script: std.Build.LazyPath,
};

pub fn init(b: *std.Build, board_dir: []const u8, sddf: []const u8) Context {
    return .{
        .b = b,
        .sddf = sddf,
        .libmicrokit = .{ .cwd_relative = b.fmt("{s}/lib/libmicrokit.a", .{board_dir}) },
        .libmicrokit_include = .{ .cwd_relative = b.fmt("{s}/include", .{board_dir}) },
        .libmicrokit_linker_script = .{ .cwd_relative = b.fmt("{s}/lib/microkit.ld", .{board_dir}) },
    };
}

pub fn addSddfIncludes(ctx: Context, mod: *std.Build.Module) void {
    mod.addIncludePath(target.sddfPath(ctx.b, ctx.sddf, "include"));
    mod.addIncludePath(target.sddfPath(ctx.b, ctx.sddf, "include/sddf/util/custom_libc"));
    mod.addIncludePath(target.sddfPath(ctx.b, ctx.sddf, "include/microkit"));
}

pub fn addPd(ctx: Context, name: []const u8, resolved_target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode) *std.Build.Step.Compile {
    const pd = ctx.b.addExecutable(.{
        .name = name,
        .root_module = ctx.b.createModule(.{ .target = resolved_target, .optimize = optimize, .strip = false }),
    });
    pd.addObjectFile(ctx.libmicrokit);
    pd.setLinkerScript(ctx.libmicrokit_linker_script);
    pd.root_module.addSystemIncludePath(ctx.libmicrokit_include);
    return pd;
}
