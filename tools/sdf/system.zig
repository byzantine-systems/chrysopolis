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
const Channel = SystemDescription.Channel;
const sddf = mod.sddf;
const lionsos = mod.lionsos;
const dtb = mod.dtb;

pub fn main() !void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const args = try std.process.argsAlloc(allocator);
    if (args.len < 4) {
        std.debug.print("usage: gen-sdf <board.dtb> <sddf-path> <output-dir> [--with-crasher] [--with-restart-debug]\n", .{});
        std.process.exit(1);
    }
    const dtb_path = args[1];
    const sddf_path = args[2];
    const out_dir = args[3];
    // Optional test-only flag: attach the deliberately faulting crasher PD as a
    // child of root, for the restart-smoke fault-injection test. Off by default,
    // so production images never carry it.
    var with_crasher = false;
    // Optional test-only flag: wire the beam_server -> root debug-restart
    // channels, one per restartable driver class. They let a test ask root to
    // restart a healthy driver on demand (via the /dev/pd-restart shim in
    // src/runtime/bringup.c) WITHOUT injecting a fault: fault detection is
    // already covered by the crasher, what the driver-restart tests need is the
    // recovery path. Off by default, so production images carry no such channel.
    var with_restart_debug = false;
    for (args[4..]) |arg| {
        if (std.mem.eql(u8, arg, "--with-crasher")) with_crasher = true;
        if (std.mem.eql(u8, arg, "--with-restart-debug")) with_restart_debug = true;
    }

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
    // The default Microkit PD stack is a single page, printf's formatting
    // frames overflow it, and ERTS's main scheduler runs on this stack
    // (pthread_create is unavailable, so it cannot move to its own). Give
    // beam_server generous room.
    beam_server.stack_size = 0x200000;
    const beam_heap = Mr.create(allocator, "beam_heap", 0x20000000, .{ .page_size = .large });
    sdf.addMemoryRegion(beam_heap);
    beam_server.addMap(Map.create(beam_heap, 0x40000000, .rw, .{ .setvar_vaddr = "beam_heap_start" }));
    sdf.addProtectionDomain(&beam_server);

    // Root fault-handler / process-manager PD. It is the PARENT of the
    // restartable driver PDs: Microkit routes a child PD's fault to its
    // parent's fault() callback (root.c), which restarts the child to a clean
    // entry (microkit_pd_restart). Priority is above every child so root can
    // preempt and handle a fault. Children are attached via root.addChild()
    // below (NOT sdf.addProtectionDomain, which would render them top-level and
    // route their faults to the monitor instead). beam_server stays top-level
    // for now; its restart is a separate issue.
    var root = Pd.create(allocator, "root", "root.elf", .{ .priority = 254 });
    sdf.addProtectionDomain(&root);

    // Child ids are PINNED rather than auto-allocated. The id is what root's
    // fault()/notified() receive to identify which driver to restart, so it is
    // an ABI between this file and src/runtime/root.c (ROOT_CHILD_*): letting
    // sdfgen allocate them would silently renumber every child whenever one is
    // added or reordered. Child ids and channel ids are SEPARATE Microkit id
    // spaces (microkit_child indexes BASE_TCB_CAP, microkit_channel indexes
    // BASE_OUTPUT_NOTIFICATION_CAP), so these never collide with channel ids.
    const CHILD_SERIAL = 0;
    const CHILD_TIMER = 1;
    const CHILD_BLK = 2;
    const CHILD_ETH = 3;
    const CHILD_CRASHER = 4; // test-only

    // Serial subsystem: PL011 driver + TX/RX virtualisers. beam_server is the
    // sole client, its console writes flow through the TX virtualiser to the
    // driver. Image names match the ELFs nix/refstack.mk builds. The driver is
    // a child of root (restartable); the virtualisers stay top-level (pure SW,
    // restarting them is out of scope here).
    var serial_driver = Pd.create(allocator, "serial_driver", "serial_driver.elf", .{ .priority = 100 });
    _ = try root.addChild(&serial_driver, .{ .id = CHILD_SERIAL });
    var serial_virt_tx = Pd.create(allocator, "serial_virt_tx", "serial_virt_tx.elf", .{ .priority = 99 });
    sdf.addProtectionDomain(&serial_virt_tx);
    var serial_virt_rx = Pd.create(allocator, "serial_virt_rx", "serial_virt_rx.elf", .{ .priority = 98 });
    sdf.addProtectionDomain(&serial_virt_rx);

    const uart_node = blob.child("pl011@9000000") orelse return error.UartNodeNotFound;
    var serial_system = try sddf.Serial.init(allocator, &sdf, uart_node, &serial_driver, &serial_virt_tx, .{ .virt_rx = &serial_virt_rx });
    try serial_system.addClient(&beam_server);

    // Timer subsystem: the ARM generic timer driver, providing the monotonic
    // clock and timeouts the LionsOS libc routes clock_gettime/nanosleep to.
    // Child of root (restartable): the monotonic clock is HW (CNTPCT), so a
    // restart re-arms the generic timer and clients re-register timeouts.
    //
    // The sDDF Timer helper forces passive=true (its connect() asserts it), but
    // a PASSIVE PD CANNOT BE RESTARTED: seL4 revokes a passive PD's scheduling
    // context after init and binds it to the PD's notification object, and per
    // the seL4 manual (S6.1.9) "the unbound thread will not be schedulable again
    // until it receives a scheduling context". microkit_pd_restart only rewrites
    // PC and resumes, leaving the child at _start with no SC. Worse, SC donation
    // over seL4_Call requires RENDEZVOUS (S6.1.11), and a PD sitting at _start is
    // not rendezvoused on its PP endpoint, so a client's SDDF_TIMER_* PPC would
    // block forever AND donate its SC into that block. So we override passive
    // back to false after timer_system.connect() (below) and give the driver its
    // own scheduling context here. PPC still works: seL4_Call to an ACTIVE
    // higher-priority PD is the ordinary case.
    //
    // Budget/period are deliberately left at the Microkit defaults, matching
    // serial_driver. Per the SDF format reference those are budget = 1,000 us and
    // period defaulting to the budget, i.e. budget == period, so the PD is never
    // throttled within a period:
    // https://docs.sel4.systems/projects/microkit/manual/latest/#sysdesc
    // The explicit budgets on the net PDs below exist because those are data-plane components
    // that sDDF rate-limits to avoid starvation collapse under load; the timer
    // is an idle IRQ handler whose notified() does a bounded scan of
    // MAX_TIMEOUTS. Throttling it would risk delaying or stalling the monotonic
    // clock every client depends on, for no benefit.
    var timer_driver = Pd.create(allocator, "timer_driver", "timer_driver.elf", .{ .priority = 101 });
    _ = try root.addChild(&timer_driver, .{ .id = CHILD_TIMER });

    // Test-only fault injector: a child of root that faults on every init, so
    // the restart-smoke test can observe root catching it, restarting it to the
    // budget, then giving up, all without touching a real driver. Present only
    // when gen-sdf is invoked with --with-crasher (the restart image). Its id is
    // pinned above, so attaching it never renumbers the real drivers.
    var crasher: Pd = undefined;
    if (with_crasher) {
        crasher = Pd.create(allocator, "crasher", "crasher.elf", .{ .priority = 50 });
        _ = try root.addChild(&crasher, .{ .id = CHILD_CRASHER });
    }

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
    // Child of root (restartable). The virtualiser stays top-level: it is pure
    // software holding the client-facing state a restart must NOT lose (the
    // reqsbk/ialloc bookkeeping that lets it error-complete requests orphaned by
    // a driver crash), so restarting it would defeat the recovery.
    var blk_driver = Pd.create(allocator, "blk_driver", "blk_driver.elf", .{ .priority = 200 });
    _ = try root.addChild(&blk_driver, .{ .id = CHILD_BLK });
    var blk_virt = Pd.create(allocator, "blk_virt", "blk_virt.elf", .{ .priority = 199 });
    sdf.addProtectionDomain(&blk_virt);

    const blk_node = blob.child("virtio_mmio@a000200") orelse return error.BlkNodeNotFound;
    var blk_system = try sddf.Blk.init(allocator, &sdf, blk_node, &blk_driver, &blk_virt, .{});

    // FAT fs_server: mounts partition 0 of the disk via the blk virtualiser and
    // serves the LionsOS fs protocol to beam_server. The FileSystem.Fat helper
    // registers fatfs as the blk client AND maps the FatFs worker-thread stacks
    // (worker_thread_stack_one..four), so we do NOT add a blk client by hand.
    // The libc fs path in beam_server stays dormant until the memfs cutover, for
    // now beam_server only verifies the share/queues are mapped at init.
    var fatfs = Pd.create(allocator, "fatfs", "fat.elf", .{ .priority = 96 });
    sdf.addProtectionDomain(&fatfs);
    var fs = try lionsos.FileSystem.Fat.init(allocator, &sdf, &fatfs, &beam_server, &blk_system, .{ .partition = 0 });

    // Network subsystem: virtio-net driver + RX/TX virtualisers + the RX
    // copier for beam_server. The DTB node is virtio_mmio@a000000, i.e. QEMU
    // virtio-mmio-bus.0, the slot reserved for ethernet (blk is pinned to
    // bus.1/a000200 above). The driver's budget/period bound its CPU time,
    // sDDF rate-limits high-priority net components to avoid starvation
    // collapse, values follow the upstream sdfgen webserver/echo examples.
    // Child of root (restartable); the net virtualisers stay top-level, same
    // reasoning as blk_virt above.
    var eth_driver = Pd.create(allocator, "eth_driver", "eth_driver.elf", .{ .priority = 110, .budget = 100, .period = 400 });
    _ = try root.addChild(&eth_driver, .{ .id = CHILD_ETH });
    var net_virt_tx = Pd.create(allocator, "net_virt_tx", "net_virt_tx.elf", .{ .priority = 109, .budget = 100, .period = 500 });
    sdf.addProtectionDomain(&net_virt_tx);
    var net_virt_rx = Pd.create(allocator, "net_virt_rx", "net_virt_rx.elf", .{ .priority = 108, .budget = 100, .period = 500 });
    sdf.addProtectionDomain(&net_virt_rx);
    var net_copy = Pd.create(allocator, "net_copy", "net_copy.elf", .{ .priority = 97, .budget = 20000 });
    sdf.addProtectionDomain(&net_copy);

    const net_node = blob.child("virtio_mmio@a000000") orelse return error.NetNodeNotFound;
    var net_system = sddf.Net.init(allocator, &sdf, net_node, &eth_driver, &net_virt_tx, &net_virt_rx, .{});
    // beam_server is the sole net client. The fixed MAC matches the NIC MAC the
    // virtio driver hardcodes, QEMU filters unicast RX by it, and keeps the
    // generated config deterministic (sdfgen otherwise randomises client MACs).
    try net_system.addClientWithCopier(&beam_server, &net_copy, .{ .mac_addr = "52:54:01:00:00:07" });

    // lwIP is linked into beam_server (not a separate PD): the sdfgen Lwip
    // helper maps a pbuf pool into beam_server and serialises the
    // .lib_sddf_lwip_config the linked lib_sddf_lwip reads at sddf_lwip_init.
    // Mirrors LionsOS posix_test's `Sddf.Lwip(sdf, net_system, client)`, must
    // follow addClientWithCopier (it depends on the net client connection) and
    // its connect() must follow net_system.connect().
    var lwip = sddf.Lwip.init(allocator, &sdf, &net_system, &beam_server);

    // Wire channels/queues/shared regions, then serialise every subsystem's
    // per-PD config blobs into out_dir (objcopied into the ELFs by the build).
    // fs.connect() registers fatfs as a blk client, so it must precede
    // blk_system.connect() (which iterates the registered clients).
    try serial_system.connect();
    try serial_system.serialiseConfig(out_dir);
    try timer_system.connect();
    // Undo the sDDF Timer helper's passive=true so the driver keeps its own
    // scheduling context and can therefore be restarted (see the timer_driver
    // declaration above for the seL4 rationale). This MUST run after connect(),
    // whose `assert(system.driver.passive.?)` would otherwise trip.
    timer_driver.passive = false;
    try timer_system.serialiseConfig(out_dir);
    try fs.connect();
    try blk_system.connect();
    try blk_system.serialiseConfig(out_dir);
    try fs.serialiseConfig(out_dir);
    try net_system.connect();
    try net_system.serialiseConfig(out_dir);
    try lwip.connect();
    try lwip.serialiseConfig(out_dir);

    // Give-up notification: root -> blk_virt, in EVERY image including
    // production. When root spends blk_driver's whole restart budget it stops
    // the driver for good, and this is how the virtualiser finds out.
    //
    // Nothing else can tell it. The driver cannot, being the thing that stopped
    // running, and the virtualiser cannot infer it: a driver that is slow to
    // come back and one that is never coming back look identical from the other
    // side of a generation counter. Left uninformed it waits forever, holding
    // every client request it had outstanding, and beam_server's
    // fs_blocking_wait() polls without yielding, so one held request wedges the
    // whole image rather than failing an fs read.
    //
    // Declared AFTER every subsystem connect()/serialiseConfig() above so it
    // cannot perturb the ids those helpers allocate, and before the debug
    // channels below purely to keep the pinned-id blocks adjacent.
    //
    // Both ends are pinned: root's id 10 is ROOT_GONE_CH_BLK in
    // src/runtime/root.c, blk_virt's is BLK_VIRT_DRIVER_GONE_CH in
    // nix/patches/sddf-blk-virt-restart-reconcile.patch. blk_virt gets a HIGH id
    // for the same reason beam_server's debug ids are high: sdfgen hands the blk
    // helper ids from 0 upwards (blk_virt already holds 0 and 1), and blk_virt
    // learns those from a serialised config blob while this channel has none, so
    // it has to be a constant that the allocator will never collide with.
    //
    // pd_b_notify = false: the signal is strictly root -> blk_virt. Denying the
    // reverse keeps root's notified() unreachable in production, which is what
    // lets the error kernel stay a pure sink for faults rather than something a
    // component below it can poke.
    sdf.addChannel(try Channel.create(&root, &blk_virt, .{
        .pd_a_id = 10,
        .pd_b_id = 61,
        .pd_b_notify = false,
    }));

    // Test-only debug-restart channels (see the --with-restart-debug comment at
    // the top). One channel per restartable driver class so the notification
    // itself carries the target: a Microkit notify has no payload, so a single
    // channel would need a shared memory word to say WHICH driver to restart.
    //
    // Both ends are pinned. Root's ids are 0..3 (it has no other channels) and
    // map 1:1 onto ROOT_DEBUG_CH_* in src/runtime/root.c. beam_server's ids are
    // pinned HIGH (58..61) and out of the way of the serial/timer/net/fs
    // channels sdfgen allocates from 0 upwards: beam_server learns every other
    // channel id from a serialised config blob, but these have no blob, so
    // src/runtime/bringup.c reads them from the .pd_restart_config section that
    // modules/images.nix objcopies in (0xff = channel absent, which is what
    // production images keep). 61 is the ceiling: sdfgen's id bitset is
    // StaticBitSet(MAX_IDS=62), so 62 is out of range and panics rather than
    // erroring. Declared last so they never perturb the ids the subsystem
    // helpers allocated above.
    if (with_restart_debug) {
        const debug_channels = [_]struct { root_id: u8, beam_id: u8 }{
            .{ .root_id = 0, .beam_id = 58 }, // serial
            .{ .root_id = 1, .beam_id = 59 }, // timer
            .{ .root_id = 2, .beam_id = 60 }, // blk
            .{ .root_id = 3, .beam_id = 61 }, // eth
        };
        for (debug_channels) |c| {
            sdf.addChannel(try Channel.create(&root, &beam_server, .{
                .pd_a_id = c.root_id,
                .pd_b_id = c.beam_id,
                // Only beam_server -> root is ever signalled (the test asks root
                // to restart a driver); root never notifies back on these.
                //
                // Deliberate, not an oversight: recovery must never depend on a
                // root -> beam_server signal, because these channels exist only
                // in the restart image (the production-sdf-gate check enforces
                // that no root <-> beam_server channel survives into production).
                // A recovery path built on this edge would work in tests and not
                // exist in production. Clients recover from what they can observe
                // on channels they already have: the timer driver notifies
                // clients whose timeouts it discarded, the blk virtualiser
                // reconciles on the generation change, and lwIP rides its own
                // timers.
                //
                // The root -> blk_virt give-up channel above is the one edge from
                // root that production DOES wire, and it is not an exception to
                // this rule: it reports something no other component is in a
                // position to observe, rather than substituting for something a
                // client could have seen for itself.
                .pd_a_notify = false,
            }));
        }
    }

    const xml = try sdf.render();
    const sdf_path = try std.fs.path.join(allocator, &.{ out_dir, "system.sdf" });
    const file = try std.fs.cwd().createFile(sdf_path, .{});
    defer file.close();
    try file.writeAll(xml);
}
