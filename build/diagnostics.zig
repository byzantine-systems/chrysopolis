const std = @import("std");

pub const runtime_contract_headers = [_][]const u8{
    "runtime_boot.h",             "runtime_config.h",       "runtime_fs.h",             "runtime_lifecycle.h",        "runtime_status.h",
    "runtime_futex_cmd.h",        "runtime_timer.h",        "runtime_wait.h",           "runtime_cothread.h",         "runtime_cothread_state.h",
    "runtime_stack.h",            "runtime_pthread_abi.h",  "runtime_pthread_handle.h", "runtime_pthread_tls.h",      "runtime_tls_row.h",
    "runtime_token_counter.h",    "runtime_thread_probe.h", "runtime_network.h",        "runtime_restart.h",          "runtime_syscalls.h",
    "runtime_syscall_handlers.h", "runtime_console.h",      "runtime_fd.h",             "runtime_fd_pair.h",          "runtime_epoll_table.h",
    "runtime_deadline.h",         "runtime_timer_slot.h",   "runtime_pd_restart.h",     "runtime_pd_restart_parse.h", "rng.h",
    "rng_select.h",               "root_policy.h",          "beam_snapshot_codec.h",    "beam_restart_layout.h",      "runtime_tcp_logic.h",
    "runtime_tcp_state.h",
};

pub fn addContractProbes(b: *std.Build, mod: *std.Build.Module, source: std.Build.LazyPath, flags: []const []const u8) void {
    for (runtime_contract_headers) |header| {
        const probe_flags = b.allocator.alloc([]const u8, flags.len + 1) catch @panic("OOM");
        @memcpy(probe_flags[0..flags.len], flags);
        probe_flags[flags.len] = b.fmt("-DRUNTIME_CONTRACT_HEADER=\"{s}\"", .{header});
        mod.addCSourceFile(.{ .file = source, .flags = probe_flags });
    }
}
