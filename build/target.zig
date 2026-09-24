const std = @import("std");

pub const BoardCfg = struct {
    serial: []const u8,
    timer: []const u8,
    blk: []const u8,
    net: []const u8,
    blk_transport: []const u8,
    net_transport: []const u8,
};

pub fn boardCfg(board: []const u8) BoardCfg {
    if (std.mem.eql(u8, board, "qemu_virt_aarch64")) {
        return .{
            .serial = "arm",
            .timer = "arm",
            .blk = "virtio",
            .net = "virtio",
            .blk_transport = "mmio",
            .net_transport = "mmio",
        };
    }
    std.debug.panic("unknown -Dboard={s}; add it to boardCfg()", .{board});
}

pub fn crossTarget(b: *std.Build) std.Build.ResolvedTarget {
    return b.resolveTargetQuery(.{
        .cpu_arch = .aarch64,
        .os_tag = .freestanding,
        .abi = .none,
        .cpu_model = .{ .explicit = &std.Target.aarch64.cpu.cortex_a53 },
        .cpu_features_add = std.Target.aarch64.featureSet(&.{.strict_align}),
    });
}

pub fn sddfPath(b: *std.Build, sddf: []const u8, sub: []const u8) std.Build.LazyPath {
    return .{ .cwd_relative = b.fmt("{s}/{s}", .{ sddf, sub }) };
}
