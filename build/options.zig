const std = @import("std");

pub const Options = struct {
    board_dir: []const u8,
    board: []const u8,
    sddf: []const u8,
    libmicrokitco_src: []const u8,
    lions_libc: []const u8,
    libc_dir: []const u8,
    lionsos_src: []const u8,
    erts_archive_dir: ?[]const u8,
    with_erts: bool,
    with_serial: bool,
    with_timer: bool,
    with_blk: bool,
    with_fs: bool,
    with_net: bool,
    with_crasher: bool,
    tcp_debug: bool,
    diagnostic: bool,

    pub fn init(b: *std.Build) Options {
        return .{
            .board_dir = b.option([]const u8, "board-dir", "Microkit board dir ($MICROKIT_SDK/board/<board>/<config>)") orelse @panic("set -Dboard-dir"),
            .board = b.option([]const u8, "board", "Microkit board name (selects driver classes)") orelse "qemu_virt_aarch64",
            .sddf = b.option([]const u8, "sddf", "sDDF source tree") orelse @panic("set -Dsddf"),
            .libmicrokitco_src = b.option([]const u8, "libmicrokitco-src", "libmicrokitco source tree") orelse @panic("set -Dlibmicrokitco-src"),
            .lions_libc = b.option([]const u8, "lions-libc", "lions-stack output (lib/libc.a + include/)") orelse @panic("set -Dlions-libc"),
            .libc_dir = b.option([]const u8, "libc-dir", "dir holding liblionsc.a (LionsOS libc.a, aliased off the special name 'c')") orelse @panic("set -Dlibc-dir"),
            .lionsos_src = b.option([]const u8, "lionsos-src", "LionsOS source tree (headers)") orelse @panic("set -Dlionsos-src"),
            .erts_archive_dir = b.option([]const u8, "erts-archive-dir", "dir holding the merged liberts_all.a (with -Dwith-erts)"),
            .with_erts = b.option(bool, "with-erts", "also build beam_test.elf (the static ERTS link)") orelse false,
            .with_serial = b.option(bool, "with-serial", "build the serial driver + virtualisers") orelse true,
            .with_timer = b.option(bool, "with-timer", "build the timer driver") orelse true,
            .with_blk = b.option(bool, "with-blk", "build the block driver + virtualiser") orelse false,
            .with_fs = b.option(bool, "with-fs", "build the FAT fs_server (fat.elf)") orelse false,
            .with_net = b.option(bool, "with-net", "build the network driver + virtualisers") orelse false,
            .with_crasher = b.option(bool, "with-crasher", "build crasher.elf (test-only faulting child of root)") orelse false,
            .tcp_debug = b.option(bool, "tcp-debug", "trace socket state transitions in tcp.c (TCP_DEBUG)") orelse false,
            .diagnostic = b.option(bool, "diagnostic", "build first-party C with ReleaseSafe and warnings as errors") orelse false,
        };
    }
};
