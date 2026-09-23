//! Typed authority and invariant checks for the existing system ABI.
//!
//! The values below are the single authority. The host ABI tool serializes
//! them to JSON for pure Nix evaluation; both Zig builds validate that
//! projection before consuming it. No missing field has a fallback value.

const std = @import("std");

pub const driver_count = 4;
pub const page_size = 0x1000;

pub const Contract = struct {
    version: u32,
    microkit: struct {
        id_count: u8,
        absent_id: u8,
    },
    children: struct {
        crasher: u8,
        beam: u8,
    },
    drivers: [driver_count]Driver,
    giveup: struct {
        root_blk_channel: u8,
        blk_virt_channel: u8,
    },
    restart: struct {
        driver_budget: u32,
        beam_budget: u32,
        entry_fallback: u64,
        exit_fault_base: u64,
        exit_fault_size: u64,
        config_section: []const u8,
        config_words: u8,
        word_bytes: u8,
        pd_config_section: []const u8,
        pd_modes: u8,
    },
    memory: struct {
        heap: Region,
        snapshot: SnapshotRegion,
    },
    sections: struct {
        serial_client: []const u8,
        timer_client: []const u8,
        fs_client: []const u8,
        net_client: []const u8,
        lwip: []const u8,
    },
    config_sections: []const ConfigSection,
};

pub const Driver = struct {
    name: []const u8,
    child: u8,
    root_debug_channel: u8,
    root_fault_channel: u8,
    beam_debug_channel: u8,
    beam_fault_channel: u8,
};

pub const Region = struct {
    size: u64,
    vaddr: u64,
    setvar: []const u8,
};

pub const SnapshotRegion = struct {
    size: u64,
    vaddr: u64,
    setvar: []const u8,
    reset_stack_offset: u64,
    reset_stack_size: u64,
    survivors_offset: u64,
    survivors_size: u64,
    data_offset: u64,
};

pub const ConfigSection = struct {
    section: []const u8,
    blob: []const u8,
    elf: []const u8,
};

pub const values: Contract = .{
    .version = 1,
    .microkit = .{ .id_count = 62, .absent_id = 255 },
    .children = .{ .crasher = 4, .beam = 5 },
    .drivers = .{
        .{ .name = "serial", .child = 0, .root_debug_channel = 0, .root_fault_channel = 4, .beam_debug_channel = 58, .beam_fault_channel = 54 },
        .{ .name = "timer", .child = 1, .root_debug_channel = 1, .root_fault_channel = 5, .beam_debug_channel = 59, .beam_fault_channel = 55 },
        .{ .name = "blk", .child = 2, .root_debug_channel = 2, .root_fault_channel = 6, .beam_debug_channel = 60, .beam_fault_channel = 56 },
        .{ .name = "eth", .child = 3, .root_debug_channel = 3, .root_fault_channel = 7, .beam_debug_channel = 61, .beam_fault_channel = 57 },
    },
    .giveup = .{ .root_blk_channel = 10, .blk_virt_channel = 61 },
    .restart = .{
        .driver_budget = 8,
        .beam_budget = 64,
        .entry_fallback = 2097152,
        .exit_fault_base = 3198156800,
        .exit_fault_size = 4096,
        .config_section = ".restart_config",
        .config_words = 2,
        .word_bytes = 8,
        .pd_config_section = ".pd_restart_config",
        .pd_modes = 2,
    },
    .memory = .{
        .heap = .{ .size = 536870912, .vaddr = 1073741824, .setvar = "beam_heap_start" },
        .snapshot = .{
            .size = 524288,
            .vaddr = 805306368,
            .setvar = "beam_snapshot_start",
            .reset_stack_offset = 4096,
            .reset_stack_size = 16384,
            .survivors_offset = 20480,
            .survivors_size = 4096,
            .data_offset = 24576,
        },
    },
    .sections = .{
        .serial_client = ".serial_client_config",
        .timer_client = ".timer_client_config",
        .fs_client = ".fs_client_config",
        .net_client = ".net_client_config",
        .lwip = ".lib_sddf_lwip_config",
    },
    .config_sections = &.{
        .{ .section = ".device_resources", .blob = "serial_driver_device_resources.data", .elf = "serial_driver.elf" },
        .{ .section = ".serial_driver_config", .blob = "serial_driver_config.data", .elf = "serial_driver.elf" },
        .{ .section = ".serial_virt_tx_config", .blob = "serial_virt_tx.data", .elf = "serial_virt_tx.elf" },
        .{ .section = ".serial_virt_rx_config", .blob = "serial_virt_rx.data", .elf = "serial_virt_rx.elf" },
        .{ .section = ".device_resources", .blob = "timer_driver_device_resources.data", .elf = "timer_driver.elf" },
        .{ .section = ".serial_client_config", .blob = "serial_client_beam_server.data", .elf = "beam_server.elf" },
        .{ .section = ".timer_client_config", .blob = "timer_client_beam_server.data", .elf = "beam_server.elf" },
        .{ .section = ".device_resources", .blob = "blk_driver_device_resources.data", .elf = "blk_driver.elf" },
        .{ .section = ".blk_driver_config", .blob = "blk_driver.data", .elf = "blk_driver.elf" },
        .{ .section = ".blk_virt_config", .blob = "blk_virt.data", .elf = "blk_virt.elf" },
        .{ .section = ".blk_client_config", .blob = "blk_client_fatfs.data", .elf = "fat.elf" },
        .{ .section = ".fs_server_config", .blob = "fs_server_fatfs.data", .elf = "fat.elf" },
        .{ .section = ".fs_client_config", .blob = "fs_client_beam_server.data", .elf = "beam_server.elf" },
        .{ .section = ".device_resources", .blob = "eth_driver_device_resources.data", .elf = "eth_driver.elf" },
        .{ .section = ".net_driver_config", .blob = "net_driver.data", .elf = "eth_driver.elf" },
        .{ .section = ".net_virt_rx_config", .blob = "net_virt_rx.data", .elf = "net_virt_rx.elf" },
        .{ .section = ".net_virt_tx_config", .blob = "net_virt_tx.data", .elf = "net_virt_tx.elf" },
        .{ .section = ".net_copy_config", .blob = "net_copy_net_copy.data", .elf = "net_copy.elf" },
        .{ .section = ".net_client_config", .blob = "net_client_beam_server.data", .elf = "beam_server.elf" },
        .{ .section = ".lib_sddf_lwip_config", .blob = "lib_sddf_lwip_config_beam_server.data", .elf = "beam_server.elf" },
    },
};

pub fn load(allocator: std.mem.Allocator, path: []const u8) !Contract {
    const bytes = try std.fs.cwd().readFileAlloc(allocator, path, 1024 * 1024);
    const contract = try std.json.parseFromSliceLeaky(Contract, allocator, bytes, .{});
    try validate(contract);
    return contract;
}

pub fn validate(contract: Contract) !void {
    if (contract.version != 1) return error.UnsupportedAbiVersion;
    if (contract.microkit.id_count != 62) return error.InvalidMicrokitIdCount;
    if (contract.microkit.absent_id < contract.microkit.id_count) return error.InvalidAbsentId;
    if (contract.restart.config_words != 2 or contract.restart.word_bytes != 8) return error.InvalidRestartConfigLayout;
    if (contract.restart.pd_modes != 2) return error.InvalidPdRestartModeCount;
    if (contract.restart.driver_budget == 0 or contract.restart.beam_budget == 0) return error.InvalidRestartBudget;
    if (contract.restart.exit_fault_size == 0 or contract.restart.exit_fault_size % page_size != 0) return error.InvalidExitFaultRange;
    if (contract.restart.exit_fault_base % page_size != 0) return error.InvalidExitFaultRange;

    const heap = contract.memory.heap;
    const snapshot = contract.memory.snapshot;
    if (heap.size == 0 or heap.size % page_size != 0 or heap.vaddr % page_size != 0) return error.InvalidHeapRegion;
    if (snapshot.size == 0 or snapshot.size % page_size != 0 or snapshot.vaddr % page_size != 0) return error.InvalidSnapshotRegion;
    if (rangesOverlap(heap.vaddr, heap.size, snapshot.vaddr, snapshot.size)) return error.OverlappingRuntimeRegions;
    if (rangesOverlap(heap.vaddr, heap.size, contract.restart.exit_fault_base, contract.restart.exit_fault_size)) return error.ExitFaultRangeMapped;
    if (rangesOverlap(snapshot.vaddr, snapshot.size, contract.restart.exit_fault_base, contract.restart.exit_fault_size)) return error.ExitFaultRangeMapped;

    const reset_stack_top = try std.math.add(u64, snapshot.reset_stack_offset, snapshot.reset_stack_size);
    const survivors_end = try std.math.add(u64, snapshot.survivors_offset, snapshot.survivors_size);
    if (snapshot.reset_stack_offset < page_size or reset_stack_top > snapshot.survivors_offset) return error.InvalidResetStackLayout;
    // src/runtime/restart.c's _reset installs this top as SP (AAPCS64 wants it
    // 16-byte aligned) and loads it with a 16-bit `mov` immediate.
    if (reset_stack_top % 16 != 0 or reset_stack_top > 0xffff) return error.InvalidResetStackLayout;
    if (survivors_end > snapshot.data_offset or snapshot.data_offset >= snapshot.size) return error.InvalidSnapshotDataLayout;

    var child_ids = [_]u8{ contract.children.crasher, contract.children.beam, 0, 0, 0, 0 };
    var root_channels: [driver_count * 2 + 1]u8 = undefined;
    var beam_channels: [driver_count * 2]u8 = undefined;
    const driver_names = [_][]const u8{ "serial", "timer", "blk", "eth" };
    for (contract.drivers, 0..) |driver, i| {
        if (!std.mem.eql(u8, driver.name, driver_names[i])) return error.InvalidDriverOrder;
        child_ids[i + 2] = driver.child;
        root_channels[i] = driver.root_debug_channel;
        root_channels[i + driver_count] = driver.root_fault_channel;
        beam_channels[i] = driver.beam_debug_channel;
        beam_channels[i + driver_count] = driver.beam_fault_channel;
        if (driver.root_debug_channel != i) return error.NonDenseRootDebugChannels;
        if (driver.root_fault_channel != i + driver_count) return error.NonDenseRootFaultChannels;
    }
    root_channels[root_channels.len - 1] = contract.giveup.root_blk_channel;
    try validateIds(&child_ids, contract.microkit.id_count);
    try validateIds(&root_channels, contract.microkit.id_count);
    try validateIds(&beam_channels, contract.microkit.id_count);
    try validateIds(&.{contract.giveup.blk_virt_channel}, contract.microkit.id_count);

    if (contract.config_sections.len == 0) return error.EmptyConfigSectionMap;
    for (contract.config_sections, 0..) |mapping, i| {
        if (mapping.section.len == 0 or mapping.section[0] != '.' or mapping.blob.len == 0 or mapping.elf.len == 0) {
            return error.InvalidConfigSectionMapping;
        }
        for (contract.config_sections[0..i]) |previous| {
            if (std.mem.eql(u8, mapping.blob, previous.blob)) return error.DuplicateConfigBlob;
            if (std.mem.eql(u8, mapping.elf, previous.elf) and std.mem.eql(u8, mapping.section, previous.section)) {
                return error.DuplicateElfSectionMapping;
            }
        }
    }
}

fn validateIds(ids: []const u8, id_count: u8) !void {
    for (ids, 0..) |id, i| {
        if (id >= id_count) return error.IdOutOfRange;
        for (ids[0..i]) |previous| {
            if (id == previous) return error.DuplicateId;
        }
    }
}

fn rangesOverlap(a_start: u64, a_size: u64, b_start: u64, b_size: u64) bool {
    const a_end = std.math.add(u64, a_start, a_size) catch return true;
    const b_end = std.math.add(u64, b_start, b_size) catch return true;
    return a_start < b_end and b_start < a_end;
}

test "typed ABI is valid and rejects conflicting values" {
    try validate(values);

    var bad = values;
    bad.children.beam = bad.children.crasher;
    try std.testing.expectError(error.DuplicateId, validate(bad));

    bad = values;
    bad.memory.snapshot.vaddr = bad.memory.heap.vaddr;
    try std.testing.expectError(error.OverlappingRuntimeRegions, validate(bad));

    bad = values;
    bad.version = 2;
    try std.testing.expectError(error.UnsupportedAbiVersion, validate(bad));
}

test "required ABI fields cannot be omitted" {
    try std.testing.expectError(error.MissingField, std.json.parseFromSliceLeaky(Contract, std.testing.allocator, "{}", .{}));
}
