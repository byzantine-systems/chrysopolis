const std = @import("std");

pub const runtime_contract_headers = [_][]const u8{
    "config/runtime_boot.h",                   "config/runtime_config.h",               "config/runtime_lifecycle.h",                "config/runtime_status.h",
    "compat/fd/runtime_fd.h",                  "compat/fd/runtime_fd_pair.h",           "compat/poll/runtime_epoll_table.h",         "compat/pthread/runtime_cothread.h",
    "compat/pthread/runtime_cothread_state.h", "compat/pthread/runtime_pthread_abi.h",  "compat/pthread/runtime_pthread_handle.h",   "compat/pthread/runtime_pthread_tls.h",
    "compat/pthread/runtime_stack.h",          "compat/pthread/runtime_thread_probe.h", "compat/pthread/runtime_tls_row.h",          "compat/pthread/runtime_token_counter.h",
    "compat/pthread/runtime_wait.h",           "compat/syscall/runtime_futex_cmd.h",    "compat/syscall/runtime_syscall_handlers.h", "compat/syscall/runtime_syscalls.h",
    "compat/time/runtime_deadline.h",          "io/console/runtime_console.h",          "io/filesystem/runtime_fs.h",                "io/network/runtime_network.h",
    "io/network/runtime_tcp_logic.h",          "io/network/runtime_tcp_state.h",        "io/timer/runtime_timer.h",                  "io/timer/runtime_timer_slot.h",
    "restart/beam_restart_layout.h",           "restart/beam_snapshot_codec.h",         "restart/runtime_pd_restart.h",              "restart/runtime_pd_restart_parse.h",
    "restart/runtime_restart.h",               "security/rng.h",                        "security/rng_select.h",
};

pub fn addContractProbes(b: *std.Build, mod: *std.Build.Module, source: std.Build.LazyPath, flags: []const []const u8) void {
    for (runtime_contract_headers) |header| {
        const probe_flags = b.allocator.alloc([]const u8, flags.len + 1) catch @panic("OOM");
        @memcpy(probe_flags[0..flags.len], flags);
        probe_flags[flags.len] = b.fmt("-DRUNTIME_CONTRACT_HEADER=\"{s}\"", .{header});
        mod.addCSourceFile(.{ .file = source, .flags = probe_flags });
    }
}
