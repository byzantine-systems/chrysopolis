//! Identifier spellings shared by the C and Erlang generators.
const std = @import("std");

const Writer = std.Io.Writer;

/// `RootStatusPage` -> `root_status_page`.
pub const Snake = struct {
    name: []const u8,
    pub fn format(self: Snake, w: *Writer) Writer.Error!void {
        for (self.name, 0..) |byte, i| {
            if (std.ascii.isUpper(byte)) {
                if (i != 0) try w.writeByte('_');
                try w.writeByte(std.ascii.toLower(byte));
            } else {
                try w.writeByte(byte);
            }
        }
    }
};

pub fn snake(name: []const u8) Snake {
    return .{ .name = name };
}

/// `abi_version` -> `AbiVersion`, an Erlang variable name.
pub const Camel = struct {
    name: []const u8,
    pub fn format(self: Camel, w: *Writer) Writer.Error!void {
        var capital = true;
        for (self.name) |byte| {
            if (byte == '_') {
                capital = true;
            } else {
                try w.writeByte(if (capital) std.ascii.toUpper(byte) else byte);
                capital = false;
            }
        }
    }
};

pub fn camel(name: []const u8) Camel {
    return .{ .name = name };
}

/// `root_status_page` -> `ROOT_STATUS_PAGE`, for Erlang macros.
pub const Upper = struct {
    name: []const u8,
    pub fn format(self: Upper, w: *Writer) Writer.Error!void {
        for (self.name, 0..) |byte, i| {
            if (std.ascii.isUpper(byte) and i != 0 and std.ascii.isLower(self.name[i - 1])) try w.writeByte('_');
            try w.writeByte(std.ascii.toUpper(byte));
        }
    }
};

pub fn upper(name: []const u8) Upper {
    return .{ .name = name };
}

test "spellings" {
    var buf: [64]u8 = undefined;
    try std.testing.expectEqualStrings("root_status_page", try std.fmt.bufPrint(&buf, "{f}", .{snake("RootStatusPage")}));
    try std.testing.expectEqualStrings("AbiVersion", try std.fmt.bufPrint(&buf, "{f}", .{camel("abi_version")}));
    try std.testing.expectEqualStrings("ROOT_STATUS_PAGE", try std.fmt.bufPrint(&buf, "{f}", .{upper("RootStatusPage")}));
    try std.testing.expectEqualStrings("EVENT_HEAD", try std.fmt.bufPrint(&buf, "{f}", .{upper("event_head")}));
}
