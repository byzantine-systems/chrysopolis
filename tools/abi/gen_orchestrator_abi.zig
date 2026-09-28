//! `gen-orchestrator-abi c|erlang <out_dir>`: validate both ABIs, then write one projection.
const std = @import("std");
const system = @import("system_abi");
const orchestration = @import("orchestrator_abi");
const model = @import("model");
const validation = @import("validation");
const generate_c = @import("generate_c");
const generate_erlang = @import("generate_erlang");

pub fn main() !void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const args = try std.process.argsAlloc(allocator);
    const Projection = enum { c, erlang };
    const projection = if (args.len == 3) std.meta.stringToEnum(Projection, args[1]) else null;
    if (projection == null) {
        std.debug.print("usage: gen-orchestrator-abi c|erlang <out_dir>\n", .{});
        return error.Usage;
    }
    var diagnostic: orchestration.Diagnostic = .{};
    validation.validateAll(system.values, model.description, &diagnostic) catch |err| {
        std.debug.print("invalid ABI {s}: {s}: {s}\n", .{ @errorName(err), diagnostic.path, diagnostic.invariant });
        return err;
    };
    var out = try std.fs.cwd().makeOpenPath(args[2], .{});
    defer out.close();
    switch (projection.?) {
        .c => try generate_c.writeAll(allocator, out),
        .erlang => try generate_erlang.writeAll(allocator, out),
    }
}
