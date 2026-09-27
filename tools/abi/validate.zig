//! Semantic gate shared by the ABI projection and its focused test fixtures.
//! The system contract owns declared topology; the orchestration contract owns
//! wire layouts. Neither declaration makes a new SDF edge or runtime mapping.
const std = @import("std");
const system = @import("system_abi");
const orchestration = @import("orchestrator_abi");
const model = @import("model");
const checks = @import("checks");

pub fn validateAll(sys: system.Contract, description: model.Description, diagnostic: *orchestration.Diagnostic) !void {
    diagnostic.* = .{};
    var system_diagnostic: system.Diagnostic = .{};
    system.validateSemantics(sys, &system_diagnostic) catch |err| {
        diagnostic.* = .{ .path = system_diagnostic.path, .invariant = system_diagnostic.invariant };
        return err;
    };
    system.validate(sys) catch |err| {
        // The pre-B2 system validator has error tags rather than a diagnostic
        // parameter. Preserve them for existing callers and supply context here.
        diagnostic.* = .{ .path = "system_abi", .invariant = @errorName(err) };
        return err;
    };
    try orchestration.validate(diagnostic);
    if (sys.control.status.size != @sizeOf(orchestration.RootStatusPage) or
        sys.control.event_ring_entries != orchestration.event_count or
        sys.microkit.id_count != orchestration.child_count or
        @offsetOf(orchestration.RootStatusPage, "events") != 5048)
    {
        diagnostic.* = .{ .path = "control.status.size", .invariant = "status page matches typed wire layout" };
        return error.InvalidCrossContract;
    }
    if (sys.control.spec.size != @sizeOf(orchestration.SpecPage) or
        sys.control.spec.header_size != @sizeOf(orchestration.SpecHeader) or
        sys.control.spec.bank_size != @sizeOf(orchestration.SpecBank))
    {
        diagnostic.* = .{ .path = "control.spec.size", .invariant = "spec page matches typed wire layout" };
        return error.InvalidCrossContract;
    }
    if (sys.pool.identity.size != @sizeOf(orchestration.WorkerIdentity) or
        sys.pool.status.size != @sizeOf(orchestration.WorkerStatus) or
        sys.pool.transport.request.size != @sizeOf(orchestration.JournalPage) or
        sys.pool.transport.completion.size != @sizeOf(orchestration.JournalPage) or
        sys.pool.transport.journal_header_size != @sizeOf(orchestration.JournalHeader) or
        sys.pool.transport.journal_entry_size != @sizeOf(orchestration.JournalEntry) or
        sys.pool.transport.journal_entries != orchestration.journal_capacity or
        sys.pool.transport.journal_payload_size != orchestration.journal_payload_size)
    {
        diagnostic.* = .{ .path = "pool.transport", .invariant = "worker pages and journals match typed wire layout" };
        return error.InvalidCrossContract;
    }
    try model.validateDescription(description, diagnostic);
    try checks.validateChecks(&checks.checkers, description, diagnostic);
}

fn expectInvalid(sys: system.Contract, expected_path: []const u8, expected_invariant: []const u8) !void {
    var diagnostic: orchestration.Diagnostic = .{};
    try std.testing.expectError(error.InvalidSemanticAbi, validateAll(sys, model.description, &diagnostic));
    try std.testing.expectEqualStrings(expected_path, diagnostic.path);
    try std.testing.expectEqualStrings(expected_invariant, diagnostic.invariant);
}

test "valid contract and deterministic repeated validation" {
    var first: orchestration.Diagnostic = .{};
    var second: orchestration.Diagnostic = .{};
    try validateAll(system.values, model.description, &first);
    try validateAll(system.values, model.description, &second);
    try std.testing.expectEqualStrings(first.path, second.path);
    try std.testing.expectEqualStrings(first.invariant, second.invariant);

    var bad = system.values;
    bad.pool.classes[0].count -= 1;
    try std.testing.expectError(error.InvalidSemanticAbi, validateAll(bad, model.description, &first));
    try std.testing.expectError(error.InvalidSemanticAbi, validateAll(bad, model.description, &second));
    try std.testing.expectEqualStrings(first.path, second.path);
    try std.testing.expectEqualStrings(first.invariant, second.invariant);
}

test "worker child range and channel collisions are rejected" {
    var bad = system.values;
    bad.pool.child_base = bad.children.beam;
    try expectInvalid(bad, "pool.child_base", "worker and existing child IDs are unique and in range");

    bad = system.values;
    bad.pool.child_base = 55;
    try expectInvalid(bad, "pool.child_base", "last worker child is below the ID count");

    bad = system.values;
    bad.pool.slots = 50;
    try expectInvalid(bad, "pool.slots", "base PDs plus workers plus crasher fit the PD limit");

    bad = system.values;
    bad.control.pp_channel.root = bad.giveup.root_blk_channel;
    try expectInvalid(bad, "control.pp_channel.root", "Root channel IDs are unique and in range");

    bad = system.values;
    bad.pool.transport.beam_channel_base = bad.drivers[0].beam_debug_channel;
    try expectInvalid(bad, "pool.transport.beam_channel_base", "worker channel IDs are in range");

    bad = system.values;
    bad.pool.transport.beam_channel_base = bad.control.pp_channel.beam - (bad.pool.slots - 1);
    try expectInvalid(bad, "pool.transport.beam_channel_base", "BEAM channel IDs are unique and in range");

    bad = system.values;
    bad.pool.transport.worker_channel = bad.microkit.id_count;
    try expectInvalid(bad, "pool.transport.worker_channel", "worker channel ID is in range");

    bad = system.values;
    bad.control.beam_dynamic_channel_floor = bad.control.pp_channel.beam + 1;
    try expectInvalid(bad, "control.beam_dynamic_channel_floor", "fixed BEAM channels are above the dynamic floor");
}

test "declared control and pool mappings are cacheable" {
    var bad = system.values;
    bad.control.status.root_cached = false;
    try expectInvalid(bad, "control.status.root_cached", "mapping is cacheable");

    bad = system.values;
    bad.control.spec.beam_cached = false;
    try expectInvalid(bad, "control.spec.beam_cached", "mapping is cacheable");

    bad = system.values;
    bad.pool.identity.worker_cached = false;
    try expectInvalid(bad, "pool.identity.worker_cached", "mapping is cacheable");

    bad = system.values;
    bad.pool.transport.request.beam_cached = false;
    try expectInvalid(bad, "pool.transport.request.beam_cached", "mapping is cacheable");
}

test "per-PD VSpace checks allow reuse only across distinct workers" {
    var bad = system.values;
    bad.control.spec.root_vaddr = bad.control.status.root_vaddr;
    try expectInvalid(bad, "control.spec.root_vaddr", "no overlap within one VSpace");

    bad = system.values;
    bad.pool.status.worker_vaddr = bad.pool.identity.worker_vaddr;
    try expectInvalid(bad, "pool.status.worker_vaddr", "no overlap within one VSpace");

    bad = system.values;
    bad.pool.identity.beam_base_vaddr = bad.memory.heap.vaddr;
    try expectInvalid(bad, "pool.identity", "no overlap within one VSpace");

    bad = system.values;
    bad.control.status.root_vaddr = bad.pool.identity.worker_vaddr;
    var diagnostic: orchestration.Diagnostic = .{};
    try validateAll(bad, model.description, &diagnostic);
    try std.testing.expectEqualStrings("", diagnostic.path);
}

test "mapping alignment, overflow and worker windows are checked" {
    var bad = system.values;
    bad.control.status.root_vaddr += 1;
    try expectInvalid(bad, "control.status.root_vaddr", "nonzero page-aligned range");

    bad = system.values;
    bad.control.status.root_vaddr = std.math.maxInt(u64) - system.page_size + 1;
    try expectInvalid(bad, "control.status.root_vaddr", "address plus size does not overflow");

    bad = system.values;
    bad.pool.beam_window_stride = std.math.maxInt(u64) - system.page_size + 1;
    try expectInvalid(bad, "pool.identity", "worker window address does not overflow");

    bad = system.values;
    bad.pool.heap.worker_vaddr = bad.pool.identity.worker_vaddr;
    try expectInvalid(bad, "pool.heap.worker_vaddr", "no overlap within one VSpace");
}

test "wire envelopes and journal capacity are exact" {
    var bad = system.values;
    bad.control.status.size = 2 * system.page_size;
    try expectInvalid(bad, "control.status.size", "status page matches wire layout and capacity");

    bad = system.values;
    bad.control.spec.bank_size += 1;
    try expectInvalid(bad, "control.spec.bank_size", "spec bank matches wire layout");

    bad = system.values;
    bad.pool.identity.size *= 2;
    try expectInvalid(bad, "pool.identity.size", "worker page matches wire envelope");

    bad = system.values;
    bad.pool.transport.journal_entries += 1;
    try expectInvalid(bad, "pool.transport.journal_entries", "journal capacity matches wire layout");
}

test "pool counts, budgets, rates and page sizes are bounded" {
    var bad = system.values;
    bad.pool.classes[0].count -= 1;
    try expectInvalid(bad, "pool.classes", "sum of class counts equals slots");

    bad = system.values;
    bad.pool.classes[0].budget = 0;
    try expectInvalid(bad, "pool.classes[0].budget", "budget is positive and at most period");

    bad = system.values;
    bad.pool.classes[1].budget = bad.pool.classes[1].period + 1;
    try expectInvalid(bad, "pool.classes[1].budget", "budget is positive and at most period");

    bad = system.values;
    bad.pool.classes[0].stack_size += 1;
    try expectInvalid(bad, "pool.classes[0].stack_size", "stack size is a nonzero page multiple");

    bad = system.values;
    bad.control.hard_budget_max = bad.restart.beam_budget - 1;
    try expectInvalid(bad, "control.hard_budget_max", "hard limit covers driver and BEAM budgets");

    bad = system.values;
    bad.control.command_rate.tokens = 0;
    try expectInvalid(bad, "control.command_rate.tokens", "rate is nonzero");
}

test "metadata fixtures reject sparse enum collisions and shifted atomic fields" {
    var diagnostic: orchestration.Diagnostic = .{};
    const values = [_]model.EnumValue{
        .{ .name = "one", .value = 1 },
        .{ .name = "four", .value = 4 },
    };
    try model.validateEnum(.{ .name = "Sparse", .width = 1, .wire = true, .values = &values }, &diagnostic);
    const duplicate = [_]model.EnumValue{ values[0], .{ .name = "another", .value = 1 } };
    try std.testing.expectError(error.DuplicateEnumValue, model.validateEnum(.{ .name = "Duplicate", .width = 1, .wire = true, .values = &duplicate }, &diagnostic));
    try std.testing.expectEqualStrings("Duplicate", diagnostic.path);
    try std.testing.expectEqualStrings("unique enum name and value", diagnostic.invariant);

    const fields = [_]model.Field{.{ .name = "seq", .offset = 1, .size = 8, .type = "u64", .element = "u64", .count = 1, .is_record = false }};
    const records = [_]model.Record{.{ .name = "Shifted", .size = 9, .alignment = 1, .fields = &fields }};
    const atomics = [_]orchestration.AtomicField{.{ .record = "Shifted", .field = "seq", .width = 8 }};
    try std.testing.expectError(error.InvalidAtomicField, model.validateAtomicFields(&atomics, &records, &diagnostic));
    try std.testing.expectEqualStrings("Shifted", diagnostic.path);
    try std.testing.expectEqualStrings("atomic width and alignment", diagnostic.invariant);

    var altered = model.description;
    altered.constants.journal_capacity += 1;
    try std.testing.expectError(error.InvalidModelConstants, model.validateDescription(altered, &diagnostic));
    try std.testing.expectEqualStrings("constants", diagnostic.path);
    try std.testing.expectEqualStrings("typed ABI version and capacities", diagnostic.invariant);
}

test "load path rejects incomplete and stale JSON projections" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    try std.testing.expectError(error.MissingField, system.load(arena.allocator(), "fixtures/missing-required.json"));
    try std.testing.expectError(error.StaleSystemAbiProjection, system.load(arena.allocator(), "fixtures/stale-config-section.json"));
    var same_content = system.values;
    const copies = try arena.allocator().dupe(system.ConfigSection, system.values.config_sections);
    copies[0].blob = try arena.allocator().dupe(u8, system.values.config_sections[0].blob);
    same_content.config_sections = copies;
    try std.testing.expect(system.contractEqual(same_content, system.values));
}
