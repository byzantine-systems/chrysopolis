//! Emit both checked ABI projections at explicit output paths.
const std = @import("std");
const serde = @import("serde");
const system = @import("system_abi");
const orchestration = @import("orchestrator_abi");
const model = @import("model");
const validation = @import("validation");

fn writeJson(allocator: std.mem.Allocator, path: []const u8, value: anytype) !void {
    const bytes = try serde.json.toSlice(allocator, value);
    const file = try std.fs.cwd().createFile(path, .{ .truncate = true });
    defer file.close();
    try file.writeAll(bytes);
    try file.writeAll("\n");
}

pub fn main() !void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const args = try std.process.argsAlloc(allocator);
    if (args.len != 3) return error.ExpectedTwoOutputPaths;
    var diagnostic: orchestration.Diagnostic = .{};
    validation.validateAll(system.values, model.description, &diagnostic) catch |err| {
        std.debug.print("invalid ABI {s}: {s}: {s}\n", .{ @errorName(err), diagnostic.path, diagnostic.invariant });
        return err;
    };
    try writeJson(allocator, args[1], system.values);
    try writeJson(allocator, args[2], model.description);
}
