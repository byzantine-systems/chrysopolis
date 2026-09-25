//! Emit both checked ABI projections at explicit output paths.
const std = @import("std");
const serde = @import("serde");
const system = @import("system_abi");
const orchestration = @import("orchestrator_abi");
const model = @import("model");

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
    try system.validate(system.values);
    var diagnostic: orchestration.Diagnostic = .{};
    try orchestration.validate(&diagnostic);
    try model.validate(&diagnostic);
    try writeJson(allocator, args[1], system.values);
    try writeJson(allocator, args[2], model.description);
}
