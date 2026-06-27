const std = @import("std");

// One root build.zig for every aarch64 cross artifact the Chrysopolis image is
// made of. It consolidates what used to be three separate Zig packages, each
// with its own build.zig and Nix derivation:
//
//   tools/libmicrokitco  -> the cooperative cothread runtime (lib/libmicrokitco.a)
//   tools/sddf-drivers   -> the sDDF driver/virtualiser Protection Domains
//   src/runtime          -> the beam_server PD glue + ERTS link (beam_*.elf)
//
// They duplicated the same boilerplate: the cross target (one
// resolveTargetQuery), the Microkit `addPd` recipe (libmicrokit + microkit.ld +
// board include), the sDDF `util`/`util_putchar_debug` libs, and the sDDF
// include set. Single-sourcing all of that here is the point of the merge.
//
// The host gen-sdf tool (tools/sdf) stays a separate package: it has a real Zig
// dependency (sdfgen) that needs zig2nix's offline cache, whereas everything
// here is dependency-free and only needs a custom Nix buildPhase
// (gen-boot-data.sh, the ERTS llvm-ar merge) the canned zig2nix builder can't do.
//
// Nix still fetches/locks every input (libc.a, the libmicrokitco/sDDF source
// trees, the Microkit SDK, the ERTS archives) and passes them in as -D options;
// Zig is only the build driver. Artifacts (all installed into one $out):
//   lib/libmicrokitco.a
//   the enabled driver/virtualiser PD ELFs (serial_driver.elf, timer_driver.elf, ...)
//   bin/beam_server.elf            (bring-up: console + clock + heap)
//   bin/beam_test.elf              (with -Dwith-erts: the same glue + static ERTS)

// Per-subsystem driver class for a board (sDDF has one source dir per class).
const BoardCfg = struct {
    serial: []const u8,
    timer: []const u8,
    blk: []const u8,
    net: []const u8,
};

fn boardCfg(board: []const u8) BoardCfg {
    if (std.mem.eql(u8, board, "qemu_virt_aarch64"))
        return .{ .serial = "arm", .timer = "arm", .blk = "virtio", .net = "virtio" };
    std.debug.panic("unknown -Dboard={s}; add it to boardCfg()", .{board});
}

// Shared link inputs for the beam_server/beam_test executables.
const BeamCfg = struct {
    glue_obj: std.Build.LazyPath,
    microkitco_obj: std.Build.LazyPath,
    board_dir: []const u8,
    lions_libc: []const u8,
    libc_dir: []const u8,
    erts_dir: ?[]const u8,
    lazy: std.Build.Module.LinkSystemLibraryOptions,
};

// The sDDF source tree (-Dsddf) and Microkit board paths (-Dboard-dir), set in
// build() and shared by the driver helpers below.
var sddf: []const u8 = undefined;
var libmicrokit: std.Build.LazyPath = undefined;
var libmicrokit_include: std.Build.LazyPath = undefined;
var libmicrokit_linker_script: std.Build.LazyPath = undefined;
// Shared across every driver PD; built once in build().
var util: *std.Build.Step.Compile = undefined;
var util_putchar_debug: *std.Build.Step.Compile = undefined;

// The single source of truth for the cross target (replaces
// `-target aarch64-none-elf -mcpu=cortex-a53 -mstrict-align`): aarch64
// freestanding, cortex-a53, strict alignment (seL4 faults on unaligned access).
fn crossTarget(b: *std.Build) std.Build.ResolvedTarget {
    return b.resolveTargetQuery(.{
        .cpu_arch = .aarch64,
        .os_tag = .freestanding,
        .abi = .none,
        .cpu_model = .{ .explicit = &std.Target.aarch64.cpu.cortex_a53 },
        .cpu_features_add = std.Target.aarch64.featureSet(&.{.strict_align}),
    });
}

fn sddfPath(b: *std.Build, sub: []const u8) std.Build.LazyPath {
    return .{ .cwd_relative = b.fmt("{s}/{s}", .{ sddf, sub }) };
}

// The shared sDDF include set every driver PD/lib compiles against.
fn addSddfIncludes(b: *std.Build, mod: *std.Build.Module) void {
    mod.addIncludePath(sddfPath(b, "include"));
    mod.addIncludePath(sddfPath(b, "include/sddf/util/custom_libc"));
    mod.addIncludePath(sddfPath(b, "include/microkit"));
}

// A Microkit PD: libmicrokit (whole archive) + the board linker script + the
// board include dir, exactly as sDDF's own addPd does.
fn addPd(
    b: *std.Build,
    name: []const u8,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) *std.Build.Step.Compile {
    const pd = b.addExecutable(.{
        .name = name,
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .strip = false,
        }),
    });
    pd.addObjectFile(libmicrokit);
    pd.setLinkerScript(libmicrokit_linker_script);
    pd.root_module.addIncludePath(libmicrokit_include);
    return pd;
}

// Build + install one sDDF component PD: its sources, the shared sDDF includes,
// any per-driver include dir, and the shared util libs.
fn component(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    name: []const u8,
    srcs: []const []const u8,
    extra_includes: []const []const u8,
) void {
    const pd = addPd(b, name, target, optimize);
    for (srcs) |s| pd.root_module.addCSourceFile(.{ .file = sddfPath(b, s) });
    addSddfIncludes(b, pd.root_module);
    for (extra_includes) |inc| pd.root_module.addIncludePath(sddfPath(b, inc));
    pd.root_module.linkLibrary(util);
    pd.root_module.linkLibrary(util_putchar_debug);
    b.installArtifact(pd);
}

// One beam_server/beam_test executable. The two variants differ only by name,
// the merged ERTS archive, and forcing the emulator entry live.
fn addBeamExe(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    cfg: BeamCfg,
    name: []const u8,
    with_erts: bool,
) void {
    const exe = b.addExecutable(.{
        .name = name,
        .root_module = b.createModule(.{
            .target = target,
            .optimize = .ReleaseFast,
            .strip = false,
        }),
    });

    // Link order mirrors the old Makefile: glue object, libmicrokitco, [ERTS],
    // then libmicrokit + libc so later archives resolve earlier references.
    // The glue is linked as an object (not module C) so it precedes the prebuilt
    // archives: Zig emits a module's own objects AFTER all link inputs, so if
    // libc.a were scanned before the glue, lld would pull file.o (libc_init_file,
    // referenced by posix.o) and collide with bringup.c's override.
    exe.root_module.addObjectFile(cfg.glue_obj);
    // libmicrokitco.a (built in this same build). addObjectFile whole-archives
    // it, pulling both libco + libmicrokitco objects.
    exe.root_module.addObjectFile(cfg.microkitco_obj);

    exe.root_module.addLibraryPath(.{ .cwd_relative = b.fmt("{s}/lib", .{cfg.board_dir}) });
    exe.root_module.addLibraryPath(.{ .cwd_relative = b.fmt("{s}/lib", .{cfg.lions_libc}) });
    exe.root_module.addLibraryPath(.{ .cwd_relative = cfg.libc_dir });

    if (with_erts) {
        exe.root_module.addLibraryPath(.{ .cwd_relative = cfg.erts_dir.? });
        exe.root_module.linkSystemLibrary("erts_all", cfg.lazy);
    }

    exe.root_module.linkSystemLibrary("microkit", cfg.lazy);
    // Zig special-cases the library name "c" (it tries to PROVIDE a libc, which
    // fails for a freestanding target), so the LionsOS libc.a is linked through
    // an alias the flake stages as liblionsc.a.
    exe.root_module.linkSystemLibrary("lionsc", cfg.lazy);

    // microkit.ld defines the memory layout + ENTRY. No PIE, no bundled
    // compiler_rt (the link supplies libc/libgcc/libmicrokitco), and no
    // --gc-sections so the config sections objcopy patches survive.
    exe.setLinkerScript(.{ .cwd_relative = b.fmt("{s}/lib/microkit.ld", .{cfg.board_dir}) });
    exe.pie = false;
    exe.bundle_compiler_rt = false;
    exe.link_gc_sections = false;
    // -u erl_start: force the emulator entry live so the ERTS archive members
    // get pulled even though main.c's reference is behind a weak symbol.
    if (with_erts) exe.forceUndefinedSymbol("erl_start");

    b.installArtifact(exe);
}

pub fn build(b: *std.Build) void {
    const target = crossTarget(b);
    const optimize: std.builtin.OptimizeMode = .ReleaseFast;

    // Nix store paths supplied by the derivation (see flake.nix).
    const board_dir = b.option([]const u8, "board-dir", "Microkit board dir ($MICROKIT_SDK/board/<board>/<config>)") orelse @panic("set -Dboard-dir");
    const board = b.option([]const u8, "board", "Microkit board name (selects driver classes)") orelse "qemu_virt_aarch64";
    sddf = b.option([]const u8, "sddf", "sDDF source tree") orelse @panic("set -Dsddf");
    const libmicrokitco_src = b.option([]const u8, "libmicrokitco-src", "libmicrokitco source tree") orelse @panic("set -Dlibmicrokitco-src");
    const lions_libc = b.option([]const u8, "lions-libc", "lions-stack output (lib/libc.a + include/)") orelse @panic("set -Dlions-libc");
    const lionsos_src = b.option([]const u8, "lionsos-src", "LionsOS source tree (headers)") orelse @panic("set -Dlionsos-src");
    const boot_data = b.option([]const u8, "boot-data", "path to the Nix-generated boot_data.c") orelse @panic("set -Dboot-data");
    const libc_dir = b.option([]const u8, "libc-dir", "dir holding liblionsc.a (LionsOS libc.a, aliased off the special name 'c')") orelse @panic("set -Dlibc-dir");
    const with_erts = b.option(bool, "with-erts", "also build beam_test.elf (the static ERTS link)") orelse false;

    const cfg = boardCfg(board);

    // Subsystem toggles. serial+timer are today's image; blk (Phase 5) and net
    // (Phase 8) are off until the SDF wires them.
    const with_serial = b.option(bool, "with-serial", "build the serial driver + virtualisers") orelse true;
    const with_timer = b.option(bool, "with-timer", "build the timer driver") orelse true;
    const with_blk = b.option(bool, "with-blk", "build the block driver + virtualiser") orelse false;
    const with_net = b.option(bool, "with-net", "build the network driver + virtualisers") orelse false;

    libmicrokit = .{ .cwd_relative = b.fmt("{s}/lib/libmicrokit.a", .{board_dir}) };
    libmicrokit_include = .{ .cwd_relative = b.fmt("{s}/include", .{board_dir}) };
    libmicrokit_linker_script = .{ .cwd_relative = b.fmt("{s}/lib/microkit.ld", .{board_dir}) };

    // === libmicrokitco: the cooperative cothread runtime ERTS's helper threads
    // spawn onto. Mirrors libmicrokitco.mk (libco/libco.c #includes the
    // arch-specific file, so it is the only libco unit; plus libmicrokitco.c).
    // The opts header is src/runtime/libmicrokitco_opts.h, on the include path
    // via b.path. Output: $out/lib/libmicrokitco.a, also linked into beam below.
    const microkitco = b.addLibrary(.{
        .name = "microkitco",
        .linkage = .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize, .strip = false }),
    });
    microkitco.root_module.addCSourceFiles(.{
        .root = .{ .cwd_relative = libmicrokitco_src },
        .files = &.{ "libco/libco.c", "libmicrokitco.c" },
        .flags = &.{ "-ffreestanding", "-O2", "-g", "-Wall" },
    });
    microkitco.root_module.addIncludePath(.{ .cwd_relative = libmicrokitco_src }); // libmicrokitco.h
    microkitco.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/libco", .{libmicrokitco_src}) }); // <libco.h>
    microkitco.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/libhostedqueue", .{libmicrokitco_src}) });
    microkitco.root_module.addIncludePath(libmicrokit_include); // microkit.h/sel4
    microkitco.root_module.addIncludePath(b.path("src/runtime")); // <libmicrokitco_opts.h>
    b.installArtifact(microkitco);

    // === sDDF drivers. util: sDDF's freestanding helpers + its custom libc
    // (incl. aarch64 asm). The drivers use this, NOT musl.
    util = b.addLibrary(.{
        .name = "util",
        .linkage = .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize }),
    });
    util.root_module.addCSourceFiles(.{
        .root = .{ .cwd_relative = sddf },
        .files = &.{
            "util/cache.c",
            "util/fsmalloc.c",
            "util/bitarray.c",
            "util/assert.c",
            "util/custom_libc/libc.c",
            "util/custom_libc/aarch64/memcmp.S",
            "util/custom_libc/aarch64/memcpy.S",
            "util/custom_libc/aarch64/memset.S",
            "util/custom_libc/aarch64/strcmp.S",
            "util/custom_libc/aarch64/strcpy.S",
            "util/custom_libc/aarch64/strlen.S",
            "util/custom_libc/aarch64/strncmp.S",
        },
    });
    addSddfIncludes(b, util.root_module);
    util.root_module.addIncludePath(libmicrokit_include);

    // util_putchar_debug: routes sDDF debug prints to the seL4 debug putchar.
    util_putchar_debug = b.addLibrary(.{
        .name = "util_putchar_debug",
        .linkage = .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize }),
    });
    util_putchar_debug.root_module.addCSourceFiles(.{
        .root = .{ .cwd_relative = sddf },
        .files = &.{ "util/assert.c", "util/printf.c", "util/putchar_debug.c" },
    });
    addSddfIncludes(b, util_putchar_debug.root_module);
    util_putchar_debug.root_module.addIncludePath(libmicrokit_include);

    // Serial: console + logging.
    if (with_serial) {
        component(b, target, optimize, "serial_driver.elf", &.{
            b.fmt("drivers/serial/{s}/uart.c", .{cfg.serial}),
        }, &.{
            b.fmt("drivers/serial/{s}/include", .{cfg.serial}),
        });
        component(b, target, optimize, "serial_virt_tx.elf", &.{"serial/components/virt_tx.c"}, &.{});
        component(b, target, optimize, "serial_virt_rx.elf", &.{"serial/components/virt_rx.c"}, &.{});
    }

    // Timer: clock.
    if (with_timer) {
        component(b, target, optimize, "timer_driver.elf", &.{
            b.fmt("drivers/timer/{s}/timer.c", .{cfg.timer}),
        }, &.{});
    }

    // Block: backs the FAT fs_server ERTS loads modules from (Phase 5).
    if (with_blk) {
        component(b, target, optimize, "blk_driver.elf", &.{
            b.fmt("drivers/blk/{s}/block.c", .{cfg.blk}),
        }, &.{
            b.fmt("drivers/blk/{s}", .{cfg.blk}),
        });
        component(b, target, optimize, "blk_virt.elf", &.{
            "blk/components/virt.c",
            "blk/components/partitioning.c",
        }, &.{});
    }

    // Network: LwIP / TCP-IP + BEAM<->BEAM distribution (Phase 8).
    if (with_net) {
        component(b, target, optimize, "eth_driver.elf", &.{
            b.fmt("drivers/network/{s}/ethernet.c", .{cfg.net}),
        }, &.{
            b.fmt("drivers/network/{s}", .{cfg.net}),
        });
        component(b, target, optimize, "net_virt_rx.elf", &.{"network/components/virt_rx.c"}, &.{});
        component(b, target, optimize, "net_virt_tx.elf", &.{"network/components/virt_tx.c"}, &.{});
        component(b, target, optimize, "net_copy.elf", &.{"network/components/copy.c"}, &.{});
    }

    // === beam_server PD glue. Compile main/bringup/process/memfs + the
    // generated boot_data.c into ONE object (see addBeamExe for why the object,
    // not module C). -O2 is pinned via cflags; ReleaseFast keeps Zig from adding
    // safety/UBSan instrumentation to the C compile.
    const cflags = &[_][]const u8{ "-ffreestanding", "-O2", "-g", "-Wall" };
    const glue = b.addObject(.{
        .name = "beam_glue",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = .ReleaseFast,
            // Microkit patches setvar_vaddr (beam_heap_start) and objcopy
            // updates the config sections, both via the symbol table.
            .strip = false,
        }),
    });
    glue.root_module.addCSourceFiles(.{
        .root = b.path("src/runtime"),
        .files = &.{ "main.c", "bringup.c", "process.c", "memfs.c" },
        .flags = cflags,
    });
    // boot_data.c is generated by tools/gen-boot-data.sh in the Nix build and
    // lives outside this package tree.
    glue.root_module.addCSourceFile(.{ .file = .{ .cwd_relative = boot_data }, .flags = cflags });

    glue.root_module.addIncludePath(b.path("src/runtime")); // libmicrokitco_opts.h, memfs.h
    glue.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/include", .{board_dir}) });
    glue.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/include", .{lions_libc}) });
    // libmicrokitco.h + libhostedqueue/ (process.c's cothread layer), straight
    // from the libmicrokitco source tree (no installed include/ needed).
    glue.root_module.addIncludePath(.{ .cwd_relative = libmicrokitco_src });
    glue.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/libhostedqueue", .{libmicrokitco_src}) });
    glue.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/include", .{sddf}) });
    glue.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/include/microkit", .{sddf}) });
    glue.root_module.addIncludePath(.{ .cwd_relative = b.fmt("{s}/include", .{lionsos_src}) });

    // Prebuilt archives are linked lazily (pulled on demand), the way the
    // Makefile's `-lmicrokit -lc` and ld --start-group did. This is a
    // correctness requirement, not just size: bringup.c strong-overrides
    // libc.a's libc_init_file, which only holds while file.o stays unextracted.
    const beam_cfg = BeamCfg{
        .glue_obj = glue.getEmittedBin(),
        .microkitco_obj = microkitco.getEmittedBin(),
        .board_dir = board_dir,
        .lions_libc = lions_libc,
        .libc_dir = libc_dir,
        .erts_dir = b.option([]const u8, "erts-archive-dir", "dir holding the merged liberts_all.a (with -Dwith-erts)"),
        .lazy = .{ .preferred_link_mode = .static, .use_pkg_config = .no },
    };

    addBeamExe(b, target, beam_cfg, "beam_server.elf", false);
    if (with_erts) {
        if (beam_cfg.erts_dir == null) @panic("set -Derts-archive-dir with -Dwith-erts");
        addBeamExe(b, target, beam_cfg, "beam_test.elf", true);
    }
}
