//! Emit the checked system ABI projection at an explicit output path.
const std = @import("std");
const serde = @import("serde");
const schema = @import("system_abi");

pub fn main() !void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const args = try std.process.argsAlloc(allocator);
    if (args.len != 2) return error.ExpectedOutputPath;
    try schema.validate(schema.values);
    const bytes = try serde.json.toSlice(allocator, schema.values);
    const file = try std.fs.cwd().createFile(args[1], .{ .truncate = true });
    defer file.close();
    try file.writeAll(bytes);
    try file.writeAll("\n");
}
