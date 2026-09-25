const std = @import("std");
const abi_schema = @import("system_abi.zig");

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

    const abi = abi_schema.load(b.allocator, "system-abi.json") catch |err| {
        std.debug.panic("invalid system-abi.json: {s}", .{@errorName(err)});
    };
    const abi_options = b.addOptions();
    abi_options.addOption(u8, "child_serial", abi.drivers[0].child);
    abi_options.addOption(u8, "child_timer", abi.drivers[1].child);
    abi_options.addOption(u8, "child_blk", abi.drivers[2].child);
    abi_options.addOption(u8, "child_eth", abi.drivers[3].child);
    abi_options.addOption(u8, "child_crasher", abi.children.crasher);
    abi_options.addOption(u8, "child_beam", abi.children.beam);
    abi_options.addOption([4]u8, "root_debug_channels", .{
        abi.drivers[0].root_debug_channel,
        abi.drivers[1].root_debug_channel,
        abi.drivers[2].root_debug_channel,
        abi.drivers[3].root_debug_channel,
    });
    abi_options.addOption([4]u8, "root_fault_channels", .{
        abi.drivers[0].root_fault_channel,
        abi.drivers[1].root_fault_channel,
        abi.drivers[2].root_fault_channel,
        abi.drivers[3].root_fault_channel,
    });
    abi_options.addOption([4]u8, "beam_debug_channels", .{
        abi.drivers[0].beam_debug_channel,
        abi.drivers[1].beam_debug_channel,
        abi.drivers[2].beam_debug_channel,
        abi.drivers[3].beam_debug_channel,
    });
    abi_options.addOption([4]u8, "beam_fault_channels", .{
        abi.drivers[0].beam_fault_channel,
        abi.drivers[1].beam_fault_channel,
        abi.drivers[2].beam_fault_channel,
        abi.drivers[3].beam_fault_channel,
    });
    abi_options.addOption(u8, "root_blk_gone_channel", abi.giveup.root_blk_channel);
    abi_options.addOption(u8, "blk_virt_gone_channel", abi.giveup.blk_virt_channel);
    abi_options.addOption(u64, "heap_size", abi.memory.heap.size);
    abi_options.addOption(u64, "heap_vaddr", abi.memory.heap.vaddr);
    abi_options.addOption([]const u8, "heap_setvar", abi.memory.heap.setvar);
    abi_options.addOption(u64, "snapshot_size", abi.memory.snapshot.size);
    abi_options.addOption(u64, "snapshot_vaddr", abi.memory.snapshot.vaddr);
    abi_options.addOption([]const u8, "snapshot_setvar", abi.memory.snapshot.setvar);
    abi_options.addOption(u64, "exit_fault_base", abi.restart.exit_fault_base);
    abi_options.addOption(u64, "exit_fault_size", abi.restart.exit_fault_size);
    abi_options.addOption(u8, "beam_dynamic_channel_floor", abi.control.beam_dynamic_channel_floor);
    abi_options.addOption(usize, "non_crasher_pd_count", abi_schema.non_crasher_pd_count);
    abi_options.addOption([2]u64, "control_beam_vaddrs", .{
        abi.control.status.beam_vaddr,
        abi.control.spec.beam_vaddr,
    });
    abi_options.addOption([2]u64, "control_sizes", .{
        abi.control.status.size,
        abi.control.spec.size,
    });
    abi_options.addOption(u8, "pool_slots", abi.pool.slots);
    abi_options.addOption(u64, "pool_beam_window_stride", abi.pool.beam_window_stride);
    abi_options.addOption([4]u64, "pool_beam_base_vaddrs", .{
        abi.pool.identity.beam_base_vaddr,
        abi.pool.status.beam_base_vaddr,
        abi.pool.transport.request.beam_base_vaddr,
        abi.pool.transport.completion.beam_base_vaddr,
    });
    abi_options.addOption([4]u64, "pool_region_sizes", .{
        abi.pool.identity.size,
        abi.pool.status.size,
        abi.pool.transport.request.size,
        abi.pool.transport.completion.size,
    });

    const exe_mod = b.createModule(.{
        .root_source_file = b.path("system.zig"),
        .target = target,
        .optimize = optimize,
    });
    exe_mod.addImport("sdf", sdfgen.module("sdf"));
    exe_mod.addOptions("runtime_abi", abi_options);

    const exe = b.addExecutable(.{
        .name = "gen-sdf",
        .root_module = exe_mod,
    });
    b.installArtifact(exe);
}
