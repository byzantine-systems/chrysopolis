const std = @import("std");
const abi_schema = @import("interfaces/system_abi.zig");
const cflags = @import("build/cflags.zig");
const components = @import("build/components.zig");
const diagnostics = @import("build/diagnostics.zig");
const microkit = @import("build/microkit.zig");
const options = @import("build/options.zig");
const pds = @import("build/pds.zig");
const target_cfg = @import("build/target.zig");

fn runtimeAbiConfigHeader(b: *std.Build, abi: abi_schema.Contract) *std.Build.Step.ConfigHeader {
    const snapshot = abi.memory.snapshot;
    const reset_stack_top = snapshot.reset_stack_offset + snapshot.reset_stack_size;
    return b.addConfigHeader(.{
        .style = .blank,
        .include_path = "runtime_abi.h",
        .include_guard_override = "CHRYSOPOLIS_RUNTIME_ABI_H",
    }, .{
        .CHRYSO_MICROKIT_ID_COUNT = @as(i64, abi.microkit.id_count),
        .ROOT_MAX_CHILDREN = @as(i64, abi.microkit.id_count),
        .ROOT_CHILD_SERIAL = @as(i64, abi.drivers[0].child),
        .ROOT_CHILD_TIMER = @as(i64, abi.drivers[1].child),
        .ROOT_CHILD_BLK = @as(i64, abi.drivers[2].child),
        .ROOT_CHILD_ETH = @as(i64, abi.drivers[3].child),
        .ROOT_CHILD_CRASHER = @as(i64, abi.children.crasher),
        .ROOT_CHILD_BEAM = @as(i64, abi.children.beam),
        .ROOT_DEBUG_CH_SERIAL = @as(i64, abi.drivers[0].root_debug_channel),
        .ROOT_DEBUG_CH_TIMER = @as(i64, abi.drivers[1].root_debug_channel),
        .ROOT_DEBUG_CH_BLK = @as(i64, abi.drivers[2].root_debug_channel),
        .ROOT_DEBUG_CH_ETH = @as(i64, abi.drivers[3].root_debug_channel),
        .ROOT_DEBUG_CH_MAX = @as(i64, abi.drivers[3].root_debug_channel),
        .ROOT_FAULT_CH_SERIAL = @as(i64, abi.drivers[0].root_fault_channel),
        .ROOT_FAULT_CH_TIMER = @as(i64, abi.drivers[1].root_fault_channel),
        .ROOT_FAULT_CH_BLK = @as(i64, abi.drivers[2].root_fault_channel),
        .ROOT_FAULT_CH_ETH = @as(i64, abi.drivers[3].root_fault_channel),
        .ROOT_FAULT_CH_MAX = @as(i64, abi.drivers[3].root_fault_channel),
        .ROOT_GONE_CH_BLK = @as(i64, abi.giveup.root_blk_channel),
        .ROOT_GONE_CH_NONE = @as(i64, abi.microkit.absent_id),
        .ROOT_RESTART_BUDGET = @as(i64, abi.restart.driver_budget),
        .ROOT_BEAM_RESTART_BUDGET = @as(i64, abi.restart.beam_budget),
        .ROOT_CLOCK_MIN_HZ = @as(i64, @intCast(abi.restart.clock_min_hz)),
        .ROOT_CLOCK_MAX_HZ = @as(i64, @intCast(abi.restart.clock_max_hz)),
        .MICROKIT_RESTART_ENTRY = @as(i64, @intCast(abi.restart.entry_fallback)),
        .ROOT_RESTART_CONFIG_WORDS = @as(i64, abi.restart.config_words),
        .ROOT_RESTART_CONFIG_WORD_BYTES = @as(i64, abi.restart.word_bytes),
        .ROOT_RESTART_CONFIG_SECTION = abi.restart.config_section,
        .PD_RESTART_CH_NONE = @as(i64, abi.microkit.absent_id),
        .PD_RESTART_CLASS_COUNT = @as(i64, abi_schema.driver_count),
        .PD_RESTART_MODE_COUNT = @as(i64, abi.restart.pd_modes),
        .PD_RESTART_MODE_HEALTHY = @as(i64, 0),
        .PD_RESTART_MODE_FAULT = @as(i64, 1),
        .PD_RESTART_CONFIG_SECTION = abi.restart.pd_config_section,
        .BEAM_HEAP_SIZE = @as(i64, @intCast(abi.memory.heap.size)),
        .BEAM_SNAPSHOT_SIZE = @as(i64, @intCast(snapshot.size)),
        .BEAM_RESET_STACK_OFF = @as(i64, @intCast(snapshot.reset_stack_offset)),
        .BEAM_RESET_STACK_SIZE = @as(i64, @intCast(snapshot.reset_stack_size)),
        .BEAM_RESET_STACK_TOP = @as(i64, @intCast(reset_stack_top)),
        .BEAM_SURVIVORS_OFF = @as(i64, @intCast(snapshot.survivors_offset)),
        .BEAM_SURVIVORS_SIZE = @as(i64, @intCast(snapshot.survivors_size)),
        .BEAM_DATA_OFF = @as(i64, @intCast(snapshot.data_offset)),
        .BEAM_DATA_SIZE = @as(i64, @intCast(snapshot.size - snapshot.data_offset)),
        .BEAM_EXIT_FAULT_BASE = @as(i64, @intCast(abi.restart.exit_fault_base)),
        .BEAM_EXIT_FAULT_SIZE = @as(i64, @intCast(abi.restart.exit_fault_size)),
        .SERIAL_CLIENT_CONFIG_SECTION = abi.sections.serial_client,
        .TIMER_CLIENT_CONFIG_SECTION = abi.sections.timer_client,
        .FS_CLIENT_CONFIG_SECTION = abi.sections.fs_client,
        .NET_CLIENT_CONFIG_SECTION = abi.sections.net_client,
        .LWIP_CONFIG_SECTION = abi.sections.lwip,
    });
}

// One root build.zig for every aarch64 cross artifact the Chrysopolis image is
// made of. It consolidates what used to be three separate Zig packages, each
// with its own build.zig and Nix derivation:
//
//   tools/libmicrokitco  -> the cooperative cothread runtime (lib/libmicrokitco.a)
//   tools/sddf-drivers   -> the sDDF driver/virtualiser Protection Domains
//   src/pd/beam          -> the beam_server PD glue + ERTS link (beam_*.elf)
//
// They duplicated the same boilerplate: the cross target (one
// resolveTargetQuery), the Microkit `addPd` recipe (libmicrokit + microkit.ld +
// board include), the sDDF `util`/`util_putchar_debug` libs, and the sDDF
// include set. Single-sourcing all of that here is the point of the merge.
//
// The host gen-sdf tool (tools/sdf) stays a separate package: it needs the
// canned zig2nix builder, whereas this build needs a custom Nix buildPhase
// (the ERTS llvm-ar merge) that builder can't do.
//
// Dependency policy: a third-party dep goes in build.zig.zon (fetched by the
// Zig package manager; zig2nix's deriveLockFile pre-populates the cache under
// Nix) iff it is (a) consumed only by this zig build, (b) an unpatched
// upstream tarball, and (c) not rev-coupled to another input. BearSSL is the
// current example. Everything else stays a Nix-provided -D option because it
// fails one of those: lionsos is patched and reconstructed (submodules +
// libc_redefine_syscall), sddf/musllibc/libmicrokitco must match the lionsos
// rev's gitlinks and are also consumed by the Nix-side musl build
// (refstack.mk) and the gen-sdf probe, the Microkit SDK is per-platform
// binaries, and the ERTS archives are a Nix cross build. Artifacts (all
// installed into one $out):
//   lib/libmicrokitco.a
//   the enabled driver/virtualiser PD ELFs (serial_driver.elf, timer_driver.elf, ...)
//   bin/root.elf                   (fault handler / process manager for the driver PDs)
//   bin/crasher.elf                (with -Dwith-crasher: test-only faulting child of root)
//   bin/beam_server.elf            (bring-up: console + clock + heap)
//   bin/beam_test.elf              (with -Dwith-erts: the same glue + static ERTS)

// The sDDF source tree (-Dsddf) and Microkit board paths (-Dboard-dir), set in
// build() and shared by the driver helpers below.
var sddf: []const u8 = undefined;
var libmicrokit: std.Build.LazyPath = undefined;
var libmicrokit_include: std.Build.LazyPath = undefined;
var libmicrokit_linker_script: std.Build.LazyPath = undefined;
var microkit_context: microkit.Context = undefined;
// Shared across every driver PD, built once in build().
var util: *std.Build.Step.Compile = undefined;
var util_putchar_debug: *std.Build.Step.Compile = undefined;

// Private BEAM headers remain with their owning subsystem. These are explicit
// because a source move must not change which headers the glue can see.
const beam_include_dirs = [_][]const u8{
    "src/pd/beam/config",
    "src/pd/beam/compat/fd",
    "src/pd/beam/compat/poll",
    "src/pd/beam/compat/pthread",
    "src/pd/beam/compat/syscall",
    "src/pd/beam/compat/time",
    "src/pd/beam/io/console",
    "src/pd/beam/io/filesystem",
    "src/pd/beam/io/network",
    "src/pd/beam/io/timer",
    "src/pd/beam/restart",
    "src/pd/beam/security",
};

// Build + install one sDDF component PD: its sources, the shared sDDF includes,
// any per-driver include dir, and the shared util libs.
fn component(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    name: []const u8,
    srcs: []const []const u8,
    extra_includes: []const []const u8,
    defines: []const []const u8,
) void {
    _ = b;
    components.add(.{
        .microkit = microkit_context,
        .util = util,
        .util_putchar_debug = util_putchar_debug,
    }, target, optimize, name, srcs, extra_includes, defines);
}

// The LionsOS FAT fs_server PD (fat.elf): FatFs (dep/ff15) + the fat component
// (event/op/io) + lib_fs_server, on its OWN libmicrokitco variant (the fat
// config's opts header caps cothreads at FAT_THREAD_NUM, distinct from beam's),
// linked against the real LionsOS libc.a, NOT the sddf util custom libc.
// Mirrors lionsos components/fs/fat/fat.mk. addPd already supplies libmicrokit,
// microkit.ld and the board include, so we don't re-link microkit here.
fn addFatServer(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    libmicrokitco_src: []const u8,
    lions_libc: []const u8,
    libc_dir: []const u8,
    lionsos_src: []const u8,
    board_dir: []const u8,
) void {
    const fat_cflags = &[_][]const u8{ "-ffreestanding", "-O2", "-g", "-Wall" };
    const fat_config_inc = b.fmt("{s}/components/fs/fat/config", .{lionsos_src});

    // A second libmicrokitco built against the fat component's opts header
    // (LIBMICROKITCO_MAX_COTHREADS = FAT_THREAD_NUM), otherwise identical to the
    // beam variant above.
    const microkitco_fat = b.addLibrary(.{
        .name = "microkitco_fat",
        .linkage = .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize, .strip = false }),
    });
    microkitco_fat.root_module.addCSourceFiles(.{
        .root = .{ .cwd_relative = libmicrokitco_src },
        .files = &.{ "libco/libco.c", "libmicrokitco.c" },
        .flags = fat_cflags,
    });
    microkitco_fat.root_module.addIncludePath(.{ .cwd_relative = libmicrokitco_src });
    microkitco_fat.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/libco", .{libmicrokitco_src}) });
    microkitco_fat.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/libhostedqueue", .{libmicrokitco_src}) });
    microkitco_fat.root_module.addIncludePath(libmicrokit_include);
    microkitco_fat.root_module.addIncludePath(.{ .cwd_relative = fat_config_inc }); // libmicrokitco_opts.h + fat_config.h

    const fat = microkit.addPd(microkit_context, "fat.elf", target, optimize);
    fat.root_module.addCSourceFiles(.{
        .root = .{ .cwd_relative = b.fmt("{s}/dep/ff15", .{lionsos_src}) },
        .files = &.{ "ff.c", "ffunicode.c" },
        .flags = fat_cflags,
    });
    fat.root_module.addCSourceFiles(.{
        .root = .{ .cwd_relative = b.fmt("{s}/components/fs/fat", .{lionsos_src}) },
        .files = &.{ "event.c", "op.c", "io.c" },
        .flags = fat_cflags,
    });
    fat.root_module.addCSourceFiles(.{
        .root = .{ .cwd_relative = b.fmt("{s}/lib/fs/server", .{lionsos_src}) },
        .files = &.{ "fd.c", "memory.c" },
        .flags = fat_cflags,
    });

    // FAT_CFLAGS include set + sddf + lions fs headers + musl libc headers.
    fat.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/dep/ff15", .{lionsos_src}) });
    fat.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/components/fs/fat", .{lionsos_src}) }); // decl.h
    fat.root_module.addIncludePath(.{ .cwd_relative = fat_config_inc }); // fat_config.h, ffconf.h
    fat.root_module.addIncludePath(.{ .cwd_relative = libmicrokitco_src }); // libmicrokitco.h
    fat.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/libhostedqueue", .{libmicrokitco_src}) });
    fat.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/include", .{sddf}) });
    fat.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/include/microkit", .{sddf}) });
    fat.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/include", .{lionsos_src}) }); // lions/fs/*
    fat.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/include", .{lions_libc}) }); // musl headers

    // Link the fat libmicrokitco variant (whole-archive), libc.a (the lionsc
    // alias), and the sddf debug putchar lib (libsddf_util_debug).
    const lazy: std.Build.Module.LinkSystemLibraryOptions = .{ .preferred_link_mode = .static, .use_pkg_config = .no };
    fat.root_module.addObjectFile(microkitco_fat.getEmittedBin());
    fat.root_module.addLibraryPath(.{ .cwd_relative = b.fmt("{s}/lib", .{board_dir}) });
    fat.root_module.addLibraryPath(.{ .cwd_relative = b.fmt("{s}/lib", .{lions_libc}) });
    fat.root_module.addLibraryPath(.{ .cwd_relative = libc_dir });
    fat.root_module.linkSystemLibrary("lionsc", lazy);
    fat.root_module.linkLibrary(util_putchar_debug);

    fat.pie = false;
    fat.bundle_compiler_rt = false;
    fat.link_gc_sections = false;
    b.installArtifact(fat);
}

// Include set for the lwIP stack and any TU that pulls
// <sddf/network/lib_sddf_lwip.h> (it transitively includes lwip/pbuf.h, which
// resolves lwipopts.h + arch/cc.h from our vendored lwip_include). Shared by
// the lib_sddf_lwip archive and beam_server's runtime_network object.
fn addLwipIncludes(
    b: *std.Build,
    mod: *std.Build.Module,
    lionsos_src: []const u8,
    lions_libc: []const u8,
    libmicrokitco_src: []const u8,
) void {
    // sDDF headers WITHOUT include/sddf/util/custom_libc: the lwIP stack + tcp.c
    // link against musl (lions_libc), so the standard headers must resolve to
    // musl's (custom_libc's stdlib.h lacks rand, which lwIP's LWIP_RAND needs).
    mod.addIncludePath(target_cfg.sddfPath(b, sddf, "include"));
    mod.addIncludePath(target_cfg.sddfPath(b, sddf, "include/microkit")); // os/sddf.h
    mod.addIncludePath(libmicrokit_include); // microkit.h
    mod.addIncludePath(.{ .cwd_relative = b.fmt("{s}/include", .{lionsos_src}) }); // lions/*
    mod.addIncludePath(.{ .cwd_relative = b.fmt("{s}/include", .{lions_libc}) }); // musl headers
    mod.addIncludePath(.{ .cwd_relative = libmicrokitco_src }); // libmicrokitco.h
    mod.addIncludePath(.{ .cwd_relative = b.fmt("{s}/libhostedqueue", .{libmicrokitco_src}) });
    mod.addIncludePath(b.path("src/pd/beam/compat/pthread")); // libmicrokitco_opts.h (beam variant)
    mod.addIncludePath(.{ .cwd_relative = b.fmt("{s}/network/ipstacks/lwip/src/include", .{sddf}) });
    mod.addIncludePath(b.path("src/pd/beam/io/network/lwip_include")); // lwipopts.h, arch/cc.h
}

// lib_sddf_lwip.a: the lwIP TCP/IP stack + sDDF glue + LionsOS socket backend,
// linked into beam_server so the prebuilt libc's socket layer (sock.c) has a
// working AF_INET path. lwIP is a LIBRARY here (not a PD), mirroring LionsOS's
// posix_test client: lib/sock/tcp.c defines the libc_socket_config_t that
// sock.c dereferences, and lib_sddf_lwip.c bridges lwIP to the sDDF net queues.
// Built as one archive.
//
// addBeamExe whole-archives it so tcp.o's socket_config is always pulled for
// the lazily-linked libc.a to resolve.
fn addLwipLib(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    lionsos_src: []const u8,
    lions_libc: []const u8,
    libmicrokitco_src: []const u8,
) *std.Build.Step.Compile {
    const lwip_src = b.fmt("{s}/network/ipstacks/lwip/src", .{sddf});
    // lwIP is noisy under -Wall, match posix_test's warning suppressions.
    const flags = &[_][]const u8{
        "-ffreestanding",
        "-O2",
        "-g",
        "-Wno-bitwise-op-parentheses",
        "-Wno-shift-op-parentheses",
        "-Wno-unused-function",
        "-Wno-tautological-constant-out-of-range-compare",
    };
    const lib = b.addLibrary(.{
        .name = "lib_sddf_lwip",
        .linkage = .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize, .strip = false }),
    });
    // lwIP core: Filelists.mk COREFILES + CORE4FILES + netif/ethernet + api/err.
    lib.root_module.addCSourceFiles(.{
        .root = .{ .cwd_relative = lwip_src },
        .files = &.{
            "core/init.c",        "core/def.c",           "core/dns.c",
            "core/inet_chksum.c", "core/ip.c",            "core/mem.c",
            "core/memp.c",        "core/netif.c",         "core/pbuf.c",
            "core/raw.c",         "core/stats.c",         "core/sys.c",
            "core/altcp.c",       "core/altcp_alloc.c",   "core/altcp_tcp.c",
            "core/tcp.c",         "core/tcp_in.c",        "core/tcp_out.c",
            "core/timeouts.c",    "core/udp.c",           "core/ipv4/acd.c",
            "core/ipv4/autoip.c", "core/ipv4/dhcp.c",     "core/ipv4/etharp.c",
            "core/ipv4/icmp.c",   "core/ipv4/igmp.c",     "core/ipv4/ip4_frag.c",
            "core/ipv4/ip4.c",    "core/ipv4/ip4_addr.c", "netif/ethernet.c",
            "api/err.c",
        },
        .flags = flags,
    });
    // sDDF<->lwIP glue.
    lib.root_module.addCSourceFile(.{
        .file = .{ .cwd_relative = b.fmt("{s}/network/lib_sddf_lwip/lib_sddf_lwip.c", .{sddf}) },
        .flags = flags,
    });
    addLwipIncludes(b, lib.root_module, lionsos_src, lions_libc, libmicrokitco_src);
    return lib;
}

// The Chrysopolis-patched TCP socket backend (it defines the socket_config
// that sock.c dereferences). Vendored at src/pd/beam/io/tcp.c: it lets recv()
// drain buffered data after the peer closes the connection (closed_by_peer),
// validates every socket index the libc layer hands over, and owns the
// conversions that cross lwIP's u16_t and u8_t boundaries.
//
// Its own object rather than a member of lib_sddf_lwip above, because it is
// code we maintain and lwIP's is not. That split is what lets it carry real
// diagnostics: -Wall -Wextra -Wsign-compare here, and -Werror under
// -Ddiagnostic, matching diagnostic_cflags.
//
// The include paths it needs are the same as lwIP's, but added with
// addSystemIncludePath so the warnings above apply to this file and not to
// the musl, seL4, Microkit and sDDF headers it pulls in, which do not compile
// clean under them and which we do not maintain. Only our BEAM directories and the
// vendored lwip_include stay plain -I: those are ours.
fn addTcpObject(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    lionsos_src: []const u8,
    lions_libc: []const u8,
    libmicrokitco_src: []const u8,
    tcp_debug: bool,
    diagnostic: bool,
) *std.Build.Step.Compile {
    const obj = b.addObject(.{
        .name = "tcp",
        .root_module = b.createModule(.{ .target = target, .optimize = optimize, .strip = false }),
    });
    // -std=gnu23: the LionsOS and seL4 headers this includes need the GNU
    // extensions. It keeps lwIP's four suppressions because it shares lwIP's
    // idioms, and they come after -Wall/-Wextra so those do not re-enable
    // what they turn off.
    const base = [_][]const u8{
        "-ffreestanding",
        "-O2",
        "-g",
        "-std=gnu23",
        "-Wall",
        "-Wextra",
        "-Wsign-compare",
        "-Wno-bitwise-op-parentheses",
        "-Wno-shift-op-parentheses",
        "-Wno-unused-function",
        "-Wno-tautological-constant-out-of-range-compare",
    };
    // The four combinations spelled out: ++ needs comptime operands and both
    // switches are build options read at runtime.
    const tcp_flags: []const []const u8 = if (diagnostic and tcp_debug)
        &(base ++ [_][]const u8{ "-Werror", "-DTCP_DEBUG=1" })
    else if (diagnostic)
        &(base ++ [_][]const u8{"-Werror"})
    else if (tcp_debug)
        &(base ++ [_][]const u8{"-DTCP_DEBUG=1"})
    else
        &base;
    obj.root_module.addCSourceFile(.{
        .file = b.path("src/pd/beam/io/tcp.c"),
        .flags = tcp_flags,
    });
    obj.root_module.addIncludePath(b.path("src/pd/beam/io/network"));
    obj.root_module.addIncludePath(b.path("src/pd/beam/config")); // runtime_network.h -> runtime_lifecycle.h
    obj.root_module.addIncludePath(b.path("src/pd/beam/compat/pthread")); // libmicrokitco_opts.h
    obj.root_module.addIncludePath(b.path("src/pd/beam/io/network/lwip_include")); // lwipopts.h, arch/cc.h
    obj.root_module.addSystemIncludePath(target_cfg.sddfPath(b, sddf, "include"));
    obj.root_module.addSystemIncludePath(target_cfg.sddfPath(b, sddf, "include/microkit")); // os/sddf.h
    obj.root_module.addSystemIncludePath(libmicrokit_include); // microkit.h
    obj.root_module.addSystemIncludePath(.{ .cwd_relative = b.fmt("{s}/include", .{lionsos_src}) });
    obj.root_module.addSystemIncludePath(.{ .cwd_relative = b.fmt("{s}/include", .{lions_libc}) });
    obj.root_module.addSystemIncludePath(.{ .cwd_relative = libmicrokitco_src });
    obj.root_module.addSystemIncludePath(.{ .cwd_relative = b.fmt("{s}/libhostedqueue", .{libmicrokitco_src}) });
    obj.root_module.addSystemIncludePath(.{ .cwd_relative = b.fmt("{s}/network/ipstacks/lwip/src/include", .{sddf}) });
    return obj;
}

pub fn build(b: *std.Build) void {
    const target = target_cfg.crossTarget(b);
    const opts = options.Options.init(b);
    const optimize: std.builtin.OptimizeMode = .ReleaseFast;
    const diagnostic = opts.diagnostic;
    const first_party_optimize: std.builtin.OptimizeMode = if (diagnostic) .ReleaseSafe else .ReleaseFast;
    const first_party_flags = cflags.firstParty(diagnostic);
    const runtime_flags = cflags.runtime(diagnostic);
    const runtime_abi = abi_schema.load(b.allocator, "interfaces/generated/system-abi.json") catch |err| {
        std.debug.panic("invalid interfaces/generated/system-abi.json: {s}", .{@errorName(err)});
    };
    const generated_abi = runtimeAbiConfigHeader(b, runtime_abi);

    const board_dir = opts.board_dir;
    sddf = opts.sddf;
    const libmicrokitco_src = opts.libmicrokitco_src;
    const lions_libc = opts.lions_libc;
    const lionsos_src = opts.lionsos_src;
    const libc_dir = opts.libc_dir;
    const with_erts = opts.with_erts;

    // BearSSL, a real Zig package dependency (build.zig.zon): a raw C source
    // tree (no build.zig), consumed via .path(). Under Nix the flake symlinks
    // zig2nix's deriveLockFile output into ZIG_GLOBAL_CACHE_DIR/p so this
    // resolves offline from the committed build.zig.zon2json-lock.
    const bearssl_dep = b.dependency("bearssl", .{});

    const cfg = target_cfg.boardCfg(opts.board);

    // Subsystem toggles. serial+timer are today's image, blk and net are off until the SDF wires them.
    const with_serial = opts.with_serial;
    const with_timer = opts.with_timer;
    const with_blk = opts.with_blk;
    const with_fs = opts.with_fs;
    const with_net = opts.with_net;
    // crasher.elf is the test-only faulting child of root. The default is false
    // for a bare `zig build`; it is NOT the production gate. modules/beam.nix
    // passes -Dwith-crasher=true unconditionally because there is one shared
    // beam-zig derivation and modules/images.nix stages crasher.elf from it for
    // the restart image only. The real gate is the SDF (--with-crasher in
    // tools/sdf/system.zig): an ELF the system description never references is
    // inert, so production images carry no crasher PD.
    const with_crasher = opts.with_crasher;
    // Socket-state tracing in src/pd/beam/io/tcp.c (TCP_DEBUG). Off by default and
    // never set by the images: it prints a line per socket event through
    // microkit_dbg_puts, which is far too chatty for a normal boot but is the
    // only way to see the ERTS/lwIP state races this layer produces. Turn it on
    // for an investigation with `zig build -Dtcp-debug=true`.
    const tcp_debug = opts.tcp_debug;

    libmicrokit = .{ .cwd_relative = b.fmt("{s}/lib/libmicrokit.a", .{board_dir}) };
    libmicrokit_include = .{ .cwd_relative = b.fmt("{s}/include", .{board_dir}) };
    libmicrokit_linker_script = .{ .cwd_relative = b.fmt("{s}/lib/microkit.ld", .{board_dir}) };
    microkit_context = microkit.init(b, board_dir, sddf);

    const lionsos = @import("build/lionsos.zig");
    const libraries = lionsos.buildLibraries(microkit_context, target, optimize, libmicrokitco_src);
    const microkitco = libraries.microkitco;
    util = libraries.util;
    util_putchar_debug = libraries.util_putchar_debug;

    // Serial: console + logging.
    if (with_serial) {
        component(b, target, optimize, "serial_driver.elf", &.{
            b.fmt("drivers/serial/{s}/uart.c", .{cfg.serial}),
        }, &.{
            b.fmt("drivers/serial/{s}/include", .{cfg.serial}),
        }, &.{});
        component(b, target, optimize, "serial_virt_tx.elf", &.{"serial/components/virt_tx.c"}, &.{}, &.{});
        component(b, target, optimize, "serial_virt_rx.elf", &.{"serial/components/virt_rx.c"}, &.{}, &.{});
    }

    // Timer: clock.
    if (with_timer) {
        component(b, target, optimize, "timer_driver.elf", &.{
            b.fmt("drivers/timer/{s}/timer.c", .{cfg.timer}),
        }, &.{}, &.{});
    }

    // Block: backs the FAT fs_server ERTS loads modules from.
    if (with_blk) {
        component(b, target, optimize, "blk_driver.elf", &.{
            b.fmt("drivers/blk/{s}/block.c", .{cfg.blk}),
            // block.c links against the virtio bus transport (mmio on qemu),
            // which sDDF builds as a separate translation unit.
            b.fmt("virtio/transport/{s}.c", .{cfg.blk_transport}),
        }, &.{
            b.fmt("drivers/blk/{s}", .{cfg.blk}),
        }, &.{});
        // DEBUG_BLK_VIRT turns on the virt's success log ("MBR partitioning
        // detected"), which is otherwise compiled out, boot-smoke gates on it.
        component(b, target, optimize, "blk_virt.elf", &.{
            "blk/components/virt.c",
            "blk/components/partitioning.c",
        }, &.{}, &.{"DEBUG_BLK_VIRT"});
    }

    // FAT filesystem server: the disk-backed fs_server beam_server's libc fs
    // path talks to (built on its own libmicrokitco variant + the real libc.a).
    if (with_fs) {
        addFatServer(b, target, optimize, libmicrokitco_src, lions_libc, libc_dir, lionsos_src, board_dir);
    }

    // Network: LwIP / TCP-IP + BEAM<->BEAM distribution.
    if (with_net) {
        component(b, target, optimize, "eth_driver.elf", &.{
            // The virtio driver body is transport-agnostic (common/) and links
            // against the bus transport (mmio on qemu), same split as blk.
            b.fmt("drivers/network/{s}/common/ethernet.c", .{cfg.net}),
            b.fmt("virtio/transport/{s}.c", .{cfg.net_transport}),
        }, &.{
            b.fmt("drivers/network/{s}/{s}", .{ cfg.net, cfg.net_transport }),
        }, &.{});
        component(b, target, optimize, "net_virt_rx.elf", &.{"network/components/virt_rx.c"}, &.{}, &.{});
        component(b, target, optimize, "net_virt_tx.elf", &.{"network/components/virt_tx.c"}, &.{}, &.{});
        component(b, target, optimize, "net_copy.elf", &.{"network/components/copy.c"}, &.{}, &.{});
    }

    // === Root fault handler and its test-only faulting child.
    // Neither is an sDDF component: Root's main.c and crasher.c include only
    // <microkit.h> and print only via microkit_dbg_puts (supplied by
    // libmicrokit.a, which addPd already links), so they need none of the sDDF
    // include set nor the util/util_putchar_debug libs that component() adds.
    // Plain addPd is the whole recipe.
    //
    // root.elf is in EVERY topology (production included: it is the fault
    // handler for the four driver PDs), so it is not behind a toggle. It reads
    // its restart entry point from the .restart_config section that
    // modules/images.nix objcopies in per board, falling back to the 0x200000
    // literal compiled in here for a bare `zig build`.
    const root_pd = microkit.addPd(microkit_context, "root.elf", target, first_party_optimize);
    root_pd.root_module.addCSourceFile(.{ .file = b.path("src/pd/root/main.c"), .flags = first_party_flags });
    root_pd.root_module.addIncludePath(b.path("src/pd/root/policy"));
    if (diagnostic) {
        root_pd.root_module.addCSourceFile(.{
            .file = b.path("src/pd/root/policy/header_probe.c"),
            .flags = runtime_flags,
        });
    }
    root_pd.root_module.addConfigHeader(generated_abi);
    // Same reason the beam exe and fat.elf disable it: modules/images.nix patches
    // the per-board child entry point into .restart_config with
    // objcopy --update-section, which fails outright if the linker garbage
    // collected the section.
    root_pd.link_gc_sections = false;
    b.installArtifact(root_pd);

    if (with_crasher) {
        // A separate ELF exercises the timed policy under QEMU. It is staged
        // only by budget-decay-image; production still uses root.elf and the
        // unchanged lifetime-only selector. Both consume the same ABI header.
        const test_root_flags = b.allocator.alloc([]const u8, first_party_flags.len + 1) catch @panic("allocating Root test flags");
        @memcpy(test_root_flags[0..first_party_flags.len], first_party_flags);
        test_root_flags[first_party_flags.len] = "-DROOT_TEST_BUDGET=1";
        const root_test_source = b.path("src/pd/root/main.c");
        const test_root = microkit.addPd(microkit_context, "root_budget_test.elf", target, first_party_optimize);
        test_root.root_module.addCSourceFile(.{ .file = root_test_source, .flags = test_root_flags });
        test_root.root_module.addIncludePath(b.path("src/pd/root/policy"));
        test_root.root_module.addConfigHeader(generated_abi);
        test_root.link_gc_sections = false;
        b.installArtifact(test_root);
    }

    if (with_crasher) {
        const crasher_pd = microkit.addPd(microkit_context, "crasher.elf", target, first_party_optimize);
        crasher_pd.root_module.addCSourceFile(.{ .file = b.path("src/pd/test_support/crasher.c"), .flags = first_party_flags });
        b.installArtifact(crasher_pd);
    }

    // === beam_server PD glue. Compile the first-party runtime into ONE object
    // (see addBeamExe for why the object, not module C). -O2 is pinned via
    // cflags. Production uses ReleaseFast; -Ddiagnostic uses ReleaseSafe and
    // -Werror. File I/O goes to the FAT fs_server (no embedded boot data).
    const glue = b.addObject(.{
        .name = "beam_glue",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = first_party_optimize,
            // Microkit patches setvar_vaddr (beam_heap_start) and objcopy
            // updates the config sections, both via the symbol table.
            .strip = false,
        }),
    });
    glue.root_module.addCSourceFiles(.{
        .root = b.path("src/pd/beam"),
        // restart.c defines _start, replacing libmicrokit.a's crt0.o. That
        // object defines only _start and references only main, so with _start
        // already defined by this object the lazily-linked libmicrokit archive
        // never extracts it and there is no duplicate symbol. It also defines
        // _reset, the entry root resumes this PD at.
        .files = &.{
            "config/c23_probe.c",
            "main.c",
            "config/runtime_lifecycle.c",
            "config/runtime_status.c",
            "config/runtime_config.c",
            "io/timer/runtime_timer.c",
            "io/filesystem/runtime_fs.c",
            "io/network/runtime_network.c",
            "payload/runtime_payload.c",
            "compat/syscall/runtime_syscalls.c",
            "io/console/runtime_console.c",
            "compat/fd/runtime_fd.c",
            "compat/fd/runtime_fd_pair.c",
            "compat/fd/runtime_sys_fd.c",
            "compat/poll/runtime_sys_poll.c",
            "compat/poll/runtime_epoll_table.c",
            "compat/time/runtime_sys_time.c",
            "compat/syscall/runtime_sys_sync.c",
            "compat/syscall/runtime_sys_identity.c",
            "compat/syscall/runtime_sys_devices.c",
            "restart/runtime_pd_restart.c",
            "restart/runtime_pd_restart_parse.c",
            "compat/pthread/runtime_cothread.c",
            "compat/pthread/runtime_stack.c",
            "compat/pthread/runtime_wait.c",
            "compat/pthread/runtime_pthread_handle.c",
            "compat/pthread/runtime_pthread.c",
            "compat/pthread/runtime_pthread_attr.c",
            "compat/pthread/runtime_pthread_tls.c",
            "compat/pthread/runtime_tls_row.c",
            "compat/pthread/runtime_pthread_locks.c",
            "compat/pthread/runtime_pthread_cond.c",
            "security/rng.c",
            "security/rng_select.c",
            "restart/restart.c",
            "restart/beam_snapshot_codec.c",
        },
        .flags = runtime_flags,
    });

    if (diagnostic) {
        glue.root_module.addCSourceFile(.{
            .file = b.path("src/pd/beam/compat/pthread/runtime_thread_probe.c"),
            .flags = runtime_flags,
        });
    }

    if (diagnostic) {
        // Compile each internal contract header alone and include it twice.
        // This rejects hidden include-order dependencies and missing guards.
        diagnostics.addContractProbes(
            b,
            glue.root_module,
            b.path("src/pd/beam/config/runtime_contract_header_probe.c"),
            runtime_flags,
        );
    }

    glue.root_module.addIncludePath(b.path("src/pd/beam")); // owner-qualified diagnostic probes
    for (beam_include_dirs) |dir| glue.root_module.addIncludePath(b.path(dir));
    glue.root_module.addConfigHeader(generated_abi);
    glue.root_module.addSystemIncludePath(.{ .cwd_relative = b.fmt("{s}/include", .{board_dir}) });
    glue.root_module.addSystemIncludePath(.{ .cwd_relative = b.fmt("{s}/include", .{lions_libc}) });
    // libmicrokitco.h + libhostedqueue/ (the cothread runtime), straight
    // from the libmicrokitco source tree (no installed include/ needed).
    glue.root_module.addSystemIncludePath(.{ .cwd_relative = libmicrokitco_src });
    glue.root_module.addSystemIncludePath(.{ .cwd_relative = b.fmt("{s}/libhostedqueue", .{libmicrokitco_src}) });
    glue.root_module.addSystemIncludePath(.{ .cwd_relative = b.fmt("{s}/include", .{sddf}) });
    glue.root_module.addSystemIncludePath(.{ .cwd_relative = b.fmt("{s}/include/microkit", .{sddf}) });
    glue.root_module.addSystemIncludePath(.{ .cwd_relative = b.fmt("{s}/include", .{lionsos_src}) });
    // lwIP headers: runtime_network.c includes this header, which pulls
    // lwip/pbuf.h -> lwipopts.h + arch/cc.h from our vendored lwip_include.
    glue.root_module.addSystemIncludePath(.{ .cwd_relative = b.fmt("{s}/network/ipstacks/lwip/src/include", .{sddf}) });
    glue.root_module.addIncludePath(b.path("src/pd/beam/io/network/lwip_include"));
    // <bearssl.h> for rng.c's HMAC-DRBG calls.
    glue.root_module.addSystemIncludePath(bearssl_dep.path("inc"));

    // Prebuilt archives are linked lazily (pulled on demand), the way the
    // Makefile's `-lmicrokit -lc` and ld --start-group did, so members are
    // extracted on demand to resolve the glue + inter-archive references.
    const lwip_lib = addLwipLib(b, target, optimize, lionsos_src, lions_libc, libmicrokitco_src);
    const tcp_obj = addTcpObject(b, target, optimize, lionsos_src, lions_libc, libmicrokitco_src, tcp_debug, diagnostic);

    // libbearssl_drbg.a: a five-file subset of BearSSL (HMAC_DRBG/SHA-256,
    // NIST SP 800-90A) backing src/pd/beam/security/rng.c. We compile only what the DRBG
    // needs, not a whole TLS stack; the deliberate choice NOT to hand-roll the
    // CSPRNG. inner.h pulls <string.h>/<limits.h> (musl, via lions_libc),
    // "config.h" (BearSSL's default, in src/) and "bearssl.h" (in inc/).
    const bearssl = b.addLibrary(.{
        .name = "bearssl_drbg",
        .linkage = .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize, .strip = false }),
    });
    bearssl.root_module.addCSourceFiles(.{
        .root = bearssl_dep.path("."),
        // hmac_drbg + hmac + SHA-256, plus the two big-endian codec helpers
        // sha2small.c calls out-of-line (br_range_enc32be/br_range_dec32be).
        .files = &.{
            "src/rand/hmac_drbg.c", "src/mac/hmac.c",      "src/hash/sha2small.c",
            "src/codec/enc32be.c",  "src/codec/dec32be.c",
        },
        .flags = &.{ "-ffreestanding", "-O2", "-g" },
    });
    bearssl.root_module.addIncludePath(bearssl_dep.path("inc"));
    bearssl.root_module.addIncludePath(bearssl_dep.path("src"));
    bearssl.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/include", .{lions_libc}) }); // musl headers

    const beam_cfg = pds.BeamCfg{
        .glue_obj = glue.getEmittedBin(),
        .microkitco_obj = microkitco.getEmittedBin(),
        .lwip_obj = lwip_lib.getEmittedBin(),
        .tcp_obj = tcp_obj.getEmittedBin(),
        .bearssl_obj = bearssl.getEmittedBin(),
        .board_dir = board_dir,
        .lions_libc = lions_libc,
        .libc_dir = libc_dir,
        .erts_dir = opts.erts_archive_dir,
        .lazy = .{ .preferred_link_mode = .static, .use_pkg_config = .no },
    };

    pds.addBeamExe(b, target, beam_cfg, "beam_server.elf", false, util_putchar_debug);
    if (with_erts) {
        if (beam_cfg.erts_dir == null) @panic("set -Derts-archive-dir with -Dwith-erts");
        pds.addBeamExe(b, target, beam_cfg, "beam_test.elf", true, util_putchar_debug);
    }
}
