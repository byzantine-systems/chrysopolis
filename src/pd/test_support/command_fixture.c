/* Test-image-only caller for Root's PPC commands. Off unless
 * modules/images.nix patches the magic into .command_fixture_config, so the
 * shared ERTS test ELF behaves as before in every other image.
 *
 * It runs from runtime_lifecycle_start, before ERTS: a PPC blocks the whole
 * beam_server TCB, schedulers included, so it must never come from a running
 * VM's hot path. The script walks a fixed step table; each step is one PPC
 * with the result it must get, and a step's precondition words come from the
 * status page or an earlier reply. */
#include "root_status_reader.h"

#include <chrysopolis/system_abi.h>
#include <runtime_abi.h>

#include <microkit.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* "CMDFIX01" little-endian; the image build writes it to enable the fixture. */
static constexpr uint64_t command_fixture_magic = UINT64_C(0x3130584946444d43);

__attribute__((__section__(".command_fixture_config"),
               used)) volatile uint64_t command_fixture_config = 0;

extern uintptr_t root_status_view;

static chryso_root_status_page fixture_scratch;
static chryso_root_status_page fixture_status;

static constexpr size_t words = 4;
static constexpr unsigned int target = ROOT_CHILD_TIMER;

void command_fixture_run(void);

typedef struct {
  chryso_ctl_result result;
  uint64_t values[words];
} reply;

typedef enum : uint8_t {
  /* Argument words as written. */
  args_literal,
  /* {target, generation, seq, target count} from the last status read. */
  args_observed,
  /* args_observed with seq moved ahead of anything Root published. */
  args_future_seq,
  /* The words of the last accepted restart, verbatim. */
  args_replay,
} args_kind;

typedef struct {
  const char *name;
  uint16_t opcode;
  uint16_t count;
  args_kind kind;
  uint64_t args[words];
  chryso_ctl_result expect;
} step;

static const step script[] = {
    {"hello",
     chryso_pp_opcode_hello,
     4,
     args_literal,
     {chryso_abi_version},
     chryso_ctl_result_ok},
    {"hello-version",
     chryso_pp_opcode_hello,
     4,
     args_literal,
     {chryso_abi_version + 1},
     chryso_ctl_result_bad_version},
    {"short",
     chryso_pp_opcode_hello,
     2,
     args_literal,
     {chryso_abi_version},
     chryso_ctl_result_bad_arg},
    {"unknown-opcode", 99, 4, args_literal, {}, chryso_ctl_result_bad_arg},
    {"undeclared-child",
     chryso_pp_opcode_restart_child,
     4,
     args_literal,
     {chryso_child_count, 1},
     chryso_ctl_result_bad_arg},
    {"beam-child",
     chryso_pp_opcode_restart_child,
     4,
     args_literal,
     {ROOT_CHILD_BEAM, 1},
     chryso_ctl_result_refused},
    {"slot",
     chryso_pp_opcode_stop_child,
     4,
     args_literal,
     {chryso_pool_child_base, 1},
     chryso_ctl_result_refused},
    {"stale",
     chryso_pp_opcode_restart_child,
     4,
     args_future_seq,
     {},
     chryso_ctl_result_stale},
    {"restart",
     chryso_pp_opcode_restart_child,
     4,
     args_observed,
     {},
     chryso_ctl_result_ok},
    {"retry",
     chryso_pp_opcode_restart_child,
     4,
     args_replay,
     {},
     chryso_ctl_result_already_done},
};

/* Root's tally of the script: accepted hello, restart, retry; rejected the
 * other seven. Every SYNC that succeeds adds one accepted. */
static constexpr uint64_t script_accepted = 3;
static constexpr uint64_t script_rejected = 7;
/* More than the bucket holds, so the flood must reach rate_limited. */
static constexpr size_t flood_limit = 2 * chryso_control_command_rate_tokens;

static void put_dec(uint64_t v) {
  char buf[21];
  size_t i = sizeof(buf);
  buf[--i] = '\0';
  do {
    buf[--i] = (char)('0' + v % 10);
    v /= 10;
  } while (v != 0);
  microkit_dbg_puts(&buf[i]);
}

static void report(const char *name, chryso_ctl_result result, bool ok) {
  microkit_dbg_puts("CMD_FIXTURE|");
  microkit_dbg_puts(name);
  microkit_dbg_puts("|result=");
  put_dec(result);
  microkit_dbg_puts(ok ? "|ok\n" : "|unexpected\n");
}

static reply call(uint16_t opcode, uint16_t count,
                  const uint64_t args[static words]) {
  for (size_t i = 0; i < words; i++) {
    microkit_mr_set((uint8_t)i, args[i]);
  }
  const microkit_msginfo info = microkit_ppcall(
      chryso_control_pp_channel_beam, microkit_msginfo_new(opcode, count));
  reply r = {.result = (chryso_ctl_result)microkit_msginfo_get_label(info)};
  if (microkit_msginfo_get_count(info) == words) {
    for (size_t i = 0; i < words; i++) {
      r.values[i] = microkit_mr_get((uint8_t)i);
    }
  }
  return r;
}

static bool read_status(void) {
  return root_status_read((const chryso_root_status_page *)root_status_view,
                          &fixture_scratch, &fixture_status,
                          sizeof(fixture_status), 3) == root_status_read_ok;
}

static void observed(uint64_t args[static words]) {
  args[0] = target;
  args[1] = fixture_status.header.root_generation;
  args[2] = fixture_status.header.seq;
  args[3] = fixture_status.children[target].lifetime_count;
}

/* Reply values a step must carry beyond its result. */
static bool values_hold(const step *s, const reply *r, const reply *accepted,
                        uint64_t observed_count, uint64_t observed_seq) {
  switch (s->kind) {
  case args_literal:
    /* Only a HELLO that passed the frame check carries Root's version. */
    if (s->opcode != chryso_pp_opcode_hello ||
        (r->result != chryso_ctl_result_ok &&
         r->result != chryso_ctl_result_bad_version)) {
      return true;
    }
    return r->values[0] == chryso_abi_version &&
           r->values[1] == fixture_status.header.root_generation;
  case args_future_seq:
    return r->values[1] == observed_seq && r->values[2] == observed_count;
  case args_observed:
    return r->values[0] == chryso_root_event_kind_restart &&
           r->values[2] == observed_count + 1 && r->values[3] > observed_seq;
  case args_replay:
    for (size_t i = 0; i < words; i++) {
      if (r->values[i] != accepted->values[i]) {
        return false;
      }
    }
    return true;
  }
  return false;
}

static bool run_script(reply *accepted) {
  uint64_t replay[words] = {};
  for (size_t n = 0; n < sizeof(script) / sizeof(script[0]); n++) {
    const step *s = &script[n];
    uint64_t args[words] = {};
    switch (s->kind) {
    case args_literal:
      for (size_t i = 0; i < words; i++) {
        args[i] = s->args[i];
      }
      break;
    case args_observed:
    case args_future_seq:
      observed(args);
      if (s->kind == args_future_seq) {
        args[2] += 2;
      }
      break;
    case args_replay:
      for (size_t i = 0; i < words; i++) {
        args[i] = replay[i];
      }
      break;
    }
    const uint64_t observed_count =
        fixture_status.children[target].lifetime_count;
    const uint64_t observed_seq = fixture_status.header.seq;
    const reply r = call(s->opcode, s->count, args);
    const bool ok = r.result == s->expect &&
                    values_hold(s, &r, accepted, observed_count, observed_seq);
    report(s->name, r.result, ok);
    if (!ok) {
      return false;
    }
    if (s->kind == args_observed) {
      *accepted = r;
      for (size_t i = 0; i < words; i++) {
        replay[i] = args[i];
      }
    }
  }
  return true;
}

/* The status page must show exactly one charged restart, published at the
 * seq the restart reply named. */
static bool check_published(const reply *accepted, uint64_t count_before) {
  const bool ok =
      read_status() &&
      fixture_status.children[target].lifetime_count == count_before + 1 &&
      fixture_status.header.seq == accepted->values[3];
  report("published", chryso_ctl_result_ok, ok);
  return ok;
}

static void flood(void) {
  static constexpr uint64_t zero[words] = {};
  uint64_t syncs = 0;
  reply last = {};
  for (size_t i = 0; i < flood_limit; i++) {
    const reply r = call(chryso_pp_opcode_sync, words, zero);
    if (r.result == chryso_ctl_result_rate_limited) {
      const bool ok = syncs > 0 && last.values[1] == script_accepted + syncs &&
                      last.values[2] == 0 && last.values[3] == script_rejected;
      report("flood", r.result, ok);
      microkit_dbg_puts("CMD_FIXTURE|syncs=");
      put_dec(syncs);
      microkit_dbg_puts("\n");
      return;
    }
    if (r.result != chryso_ctl_result_ok) {
      report("flood-sync", r.result, false);
      return;
    }
    syncs++;
    last = r;
  }
  report("flood-unlimited", chryso_ctl_result_ok, false);
}

void command_fixture_run(void) {
  if (command_fixture_config != command_fixture_magic) {
    return;
  }
  if (root_status_view != chryso_control_status_beam_vaddr) {
    microkit_dbg_puts("CMD_FIXTURE|invalid-map\n");
    return;
  }
  if (!read_status()) {
    microkit_dbg_puts("CMD_FIXTURE|status-unreadable\n");
    return;
  }
  if (fixture_status.header.beam_incarnation != 1) {
    microkit_dbg_puts("CMD_FIXTURE|later\n");
    return;
  }
  const uint64_t count_before = fixture_status.children[target].lifetime_count;
  reply accepted = {};
  if (!run_script(&accepted) || !check_published(&accepted, count_before)) {
    return;
  }
  flood();
  microkit_dbg_puts("CMD_FIXTURE|done\n");
}
