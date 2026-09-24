const std = @import("std");
const microkit = @import("microkit.zig");
const target = @import("target.zig");

pub const Context = struct {
    microkit: microkit.Context,
    util: *std.Build.Step.Compile,
    util_putchar_debug: *std.Build.Step.Compile,
};

pub fn add(
    ctx: Context,
    resolved_target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    name: []const u8,
    srcs: []const []const u8,
    extra_includes: []const []const u8,
    defines: []const []const u8,
) void {
    const pd = microkit.addPd(ctx.microkit, name, resolved_target, optimize);
    for (srcs) |source| pd.root_module.addCSourceFile(.{ .file = target.sddfPath(ctx.microkit.b, ctx.microkit.sddf, source) });
    microkit.addSddfIncludes(ctx.microkit, pd.root_module);
    for (extra_includes) |include| pd.root_module.addIncludePath(target.sddfPath(ctx.microkit.b, ctx.microkit.sddf, include));
    for (defines) |define| pd.root_module.addCMacro(define, "1");
    pd.root_module.linkLibrary(ctx.util);
    pd.root_module.linkLibrary(ctx.util_putchar_debug);
    ctx.microkit.b.installArtifact(pd);
}
