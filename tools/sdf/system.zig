//! Programmatic generator for the Chrysopolis Microkit system description.
//!
//! Plan B topology: the BEAM server runs as a Microkit PD linked against the
//! LionsOS POSIX libc, talking to real sDDF driver PDs. We build the serial
//! (console) and timer (clock) subsystems with sdfgen's high-level helpers,
//! which both render the .sdf AND serialise the per-PD config blobs the
//! reference stack needs (driver device-resources, virtualiser configs,
//! per-client configs). The build step objcopies those .data blobs into the
//! matching ELF sections.
//!
//! The boot heap is kept as a dedicated memory region mapped into beam_server
//! with setvar_vaddr="beam_heap_start" (ADR 01): the C runtime hands that
//! region to libc_init() as the malloc arena rather than baking a BSS array
//! into the ELF.
//!
//! Invoked at build time:
//!   gen-sdf <board.dtb> <sddf-source-path> <output-dir>
//! Writes <output-dir>/system.sdf plus the subsystem *.data config blobs.

const std = @import("std");

const mod = @import("sdf");
const SystemDescription = mod.sdf.SystemDescription;
const Pd = SystemDescription.ProtectionDomain;
const Mr = SystemDescription.MemoryRegion;
const Map = SystemDescription.Map;
const sddf = mod.sddf;
const lionsos = mod.lionsos;
const dtb = mod.dtb;

pub fn main() !void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const args = try std.process.argsAlloc(allocator);
    if (args.len < 4) {
        std.debug.print("usage: gen-sdf <board.dtb> <sddf-path> <output-dir>\n", .{});
        std.process.exit(1);
    }
    const dtb_path = args[1];
    const sddf_path = args[2];
    const out_dir = args[3];

    // Parse the board device tree (compiled from sDDF's own dts) so the
    // helpers can resolve the UART and timer device nodes, IRQs and MMIO.
    const dtb_file = try std.fs.cwd().openFile(dtb_path, .{});
    const dtb_bytes = try dtb_file.readToEndAlloc(allocator, 1024 * 1024);
    var blob = try dtb.parse(allocator, dtb_bytes);
    defer blob.deinit(allocator);

    // sdfgen scans the sDDF source tree for driver metadata (config.json).
    try sddf.probe(allocator, sddf_path);

    // paddr_top for qemu_virt_aarch64: top of RAM sdfgen allocates physical
    // pages downward from, matching the LionsOS sdfgen examples.
    var sdf = SystemDescription.create(allocator, .aarch64, 0xa0000000);

    // The BEAM server PD and its malloc arena. The map carries the
    // setvar_vaddr symbol the Microkit tool patches into beam_server.elf, so
    // the C runtime reads the heap base from beam_heap_start (ADR 01).
    var beam_server = Pd.create(allocator, "beam_server", "beam_server.elf", .{ .priority = 1 });
    // The default Microkit PD stack is a single page; printf's formatting
    // frames overflow it, and ERTS's main scheduler runs on this stack
    // (pthread_create is unavailable, so it cannot move to its own). Give
    // beam_server generous room.
    beam_server.stack_size = 0x200000;
    const beam_heap = Mr.create(allocator, "beam_heap", 0x20000000, .{ .page_size = .large });
    sdf.addMemoryRegion(beam_heap);
    beam_server.addMap(Map.create(beam_heap, 0x40000000, .rw, .{ .setvar_vaddr = "beam_heap_start" }));
    sdf.addProtectionDomain(&beam_server);

    // Serial subsystem: PL011 driver + TX/RX virtualisers. beam_server is the
    // sole client; its console writes flow through the TX virtualiser to the
    // driver. Image names match the ELFs nix/refstack.mk builds.
    var serial_driver = Pd.create(allocator, "serial_driver", "serial_driver.elf", .{ .priority = 100 });
    sdf.addProtectionDomain(&serial_driver);
    var serial_virt_tx = Pd.create(allocator, "serial_virt_tx", "serial_virt_tx.elf", .{ .priority = 99 });
    sdf.addProtectionDomain(&serial_virt_tx);
    var serial_virt_rx = Pd.create(allocator, "serial_virt_rx", "serial_virt_rx.elf", .{ .priority = 98 });
    sdf.addProtectionDomain(&serial_virt_rx);

    const uart_node = blob.child("pl011@9000000") orelse return error.UartNodeNotFound;
    var serial_system = try sddf.Serial.init(allocator, &sdf, uart_node, &serial_driver, &serial_virt_tx, .{ .virt_rx = &serial_virt_rx });
    try serial_system.addClient(&beam_server);

    // Timer subsystem: the ARM generic timer driver, providing the monotonic
    // clock and timeouts the LionsOS libc routes clock_gettime/nanosleep to.
    var timer_driver = Pd.create(allocator, "timer_driver", "timer_driver.elf", .{ .priority = 101 });
    sdf.addProtectionDomain(&timer_driver);

    const timer_node = blob.child("timer") orelse return error.TimerNodeNotFound;
    var timer_system = sddf.Timer.init(allocator, &sdf, timer_node, &timer_driver);
    try timer_system.addClient(&beam_server);

    // Block + FAT filesystem: TEMPORARILY DISABLED (Option C)
    // Testing ERTS boot without filesystem to confirm threading/syscalls work
    // Will implement minimal in-memory boot environment as workaround
    // var blk_driver = Pd.create(allocator, "blk_driver", "blk_driver.elf", .{ .priority = 200 });
    // var blk_virt = Pd.create(allocator, "blk_virt", "blk_virt.elf", .{ .priority = 199 });
    // var fatfs = Pd.create(allocator, "fatfs", "fat.elf", .{ .priority = 96 });
    // const blk_node = blob.child("virtio_mmio@a000000") orelse return error.BlkNodeNotFound;
    // var blk_system = try sddf.Blk.init(allocator, &sdf, blk_node, &blk_driver, &blk_virt, .{});
    // var fs = try lionsos.FileSystem.init(allocator, &sdf, &fatfs, &beam_server, .{});

    // Wire channels/queues/shared regions, then serialise every subsystem's
    // per-PD config blobs into out_dir (objcopied into the ELFs by the build).
    try serial_system.connect();
    try serial_system.serialiseConfig(out_dir);
    try timer_system.connect();
    try timer_system.serialiseConfig(out_dir);
    // try blk_system.connect();
    // try blk_system.serialiseConfig(out_dir);
    // fs.connect(.{});
    // try fs.serialiseConfig(out_dir);

    const xml = try sdf.render();
    const sdf_path = try std.fs.path.join(allocator, &.{ out_dir, "system.sdf" });
    const file = try std.fs.cwd().createFile(sdf_path, .{});
    defer file.close();
    try file.writeAll(xml);
}
