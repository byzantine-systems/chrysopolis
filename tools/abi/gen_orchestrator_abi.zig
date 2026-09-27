//! `gen-orchestrator-abi c <out_dir>`: validate both ABIs, then write the C headers.
const std = @import("std");
const system = @import("system_abi");
const orchestration = @import("orchestrator_abi");
const model = @import("model");
const validation = @import("validation");
const generate_c = @import("generate_c");

pub fn main() !void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const args = try std.process.argsAlloc(allocator);
    if (args.len != 3 or !std.mem.eql(u8, args[1], "c")) {
        std.debug.print("usage: gen-orchestrator-abi c <out_dir>\n", .{});
        return error.Usage;
    }
    var diagnostic: orchestration.Diagnostic = .{};
    validation.validateAll(system.values, model.description, &diagnostic) catch |err| {
        std.debug.print("invalid ABI {s}: {s}: {s}\n", .{ @errorName(err), diagnostic.path, diagnostic.invariant });
        return err;
    };
    var out = try std.fs.cwd().makeOpenPath(args[2], .{});
    defer out.close();
    try generate_c.writeAll(allocator, out);
}
