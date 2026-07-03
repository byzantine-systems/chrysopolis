//! Programmatic generator for the Chrysopolis Microkit system description.
//!
//! Topology: the BEAM server runs as a Microkit PD linked against the
//! LionsOS POSIX libc, talking to real sDDF driver PDs. We build the serial
//! (console), timer (clock), block (FAT disk) and network (ethernet)
//! subsystems with sdfgen's high-level helpers, which both render the .sdf
//! AND serialise the per-PD config blobs the reference stack needs (driver
//! device-resources, virtualiser configs, per-client configs). The build
//! step objcopies those .data blobs into the matching ELF sections.
//!
//! The boot heap is kept as a dedicated memory region mapped into beam_server
//! with setvar_vaddr="beam_heap_start": the C runtime hands that region to
//! libc_init() as the malloc arena rather than baking a BSS array into the ELF.
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
    // the C runtime reads the heap base from beam_heap_start.
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

    // Block subsystem: virtio-mmio block driver + block virtualiser. The FAT
    // fs_server (fatfs, below) is the sole blk client, on partition 0.
    //
    // The DTB node is virtio_mmio@a000200 (IRQ 17), matching the LionsOS board
    // config (sddf tools/meta/board.py: blk="virtio_mmio@a000200") and the QEMU
    // attach `bus=virtio-mmio-bus.1`. QEMU maps virtio-mmio-bus.N to address
    // 0xa000000 + N*0x200, so bus.1 == a000200. (a000000/bus.0 is reserved for
    // ethernet in that config.) Pinning the bus keeps the disk on a fixed slot
    // rather than relying on QEMU's highest-slot-first auto-placement.
    var blk_driver = Pd.create(allocator, "blk_driver", "blk_driver.elf", .{ .priority = 200 });
    sdf.addProtectionDomain(&blk_driver);
    var blk_virt = Pd.create(allocator, "blk_virt", "blk_virt.elf", .{ .priority = 199 });
    sdf.addProtectionDomain(&blk_virt);

    const blk_node = blob.child("virtio_mmio@a000200") orelse return error.BlkNodeNotFound;
    var blk_system = try sddf.Blk.init(allocator, &sdf, blk_node, &blk_driver, &blk_virt, .{});

    // FAT fs_server: mounts partition 0 of the disk via the blk virtualiser and
    // serves the LionsOS fs protocol to beam_server. The FileSystem.Fat helper
    // registers fatfs as the blk client AND maps the FatFs worker-thread stacks
    // (worker_thread_stack_one..four), so we do NOT add a blk client by hand.
    // The libc fs path in beam_server stays dormant until the memfs cutover; for
    // now beam_server only verifies the share/queues are mapped at init.
    var fatfs = Pd.create(allocator, "fatfs", "fat.elf", .{ .priority = 96 });
    sdf.addProtectionDomain(&fatfs);
    var fs = try lionsos.FileSystem.Fat.init(allocator, &sdf, &fatfs, &beam_server, &blk_system, .{ .partition = 0 });

    // Network subsystem: virtio-net driver + RX/TX virtualisers + the RX
    // copier for beam_server. The DTB node is virtio_mmio@a000000, i.e. QEMU
    // virtio-mmio-bus.0, the slot reserved for ethernet (blk is pinned to
    // bus.1/a000200 above). The driver's budget/period bound its CPU time,
    // sDDF rate-limits high-priority net components to avoid starvation
    // collapse; values follow the upstream sdfgen webserver/echo examples.
    var eth_driver = Pd.create(allocator, "eth_driver", "eth_driver.elf", .{ .priority = 110, .budget = 100, .period = 400 });
    sdf.addProtectionDomain(&eth_driver);
    var net_virt_tx = Pd.create(allocator, "net_virt_tx", "net_virt_tx.elf", .{ .priority = 109, .budget = 100, .period = 500 });
    sdf.addProtectionDomain(&net_virt_tx);
    var net_virt_rx = Pd.create(allocator, "net_virt_rx", "net_virt_rx.elf", .{ .priority = 108, .budget = 100, .period = 500 });
    sdf.addProtectionDomain(&net_virt_rx);
    var net_copy = Pd.create(allocator, "net_copy", "net_copy.elf", .{ .priority = 97, .budget = 20000 });
    sdf.addProtectionDomain(&net_copy);

    const net_node = blob.child("virtio_mmio@a000000") orelse return error.NetNodeNotFound;
    var net_system = sddf.Net.init(allocator, &sdf, net_node, &eth_driver, &net_virt_tx, &net_virt_rx, .{});
    // beam_server is the sole net client (the lwIP socket layer lands next
    // milestone; until then its .net_client_config stays un-embedded). The
    // fixed MAC matches the NIC MAC the virtio driver hardcodes, QEMU filters
    // unicast RX by it, and keeps the generated config deterministic (sdfgen
    // otherwise randomises client MACs).
    try net_system.addClientWithCopier(&beam_server, &net_copy, .{ .mac_addr = "52:54:01:00:00:07" });

    // Wire channels/queues/shared regions, then serialise every subsystem's
    // per-PD config blobs into out_dir (objcopied into the ELFs by the build).
    // fs.connect() registers fatfs as a blk client, so it must precede
    // blk_system.connect() (which iterates the registered clients).
    try serial_system.connect();
    try serial_system.serialiseConfig(out_dir);
    try timer_system.connect();
    try timer_system.serialiseConfig(out_dir);
    try fs.connect();
    try blk_system.connect();
    try blk_system.serialiseConfig(out_dir);
    try fs.serialiseConfig(out_dir);
    try net_system.connect();
    try net_system.serialiseConfig(out_dir);

    const xml = try sdf.render();
    const sdf_path = try std.fs.path.join(allocator, &.{ out_dir, "system.sdf" });
    const file = try std.fs.cwd().createFile(sdf_path, .{});
    defer file.close();
    try file.writeAll(xml);
}
