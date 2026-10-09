/* Field-by-field little-endian stores, so the page bytes never depend on the
 * writer's struct layout. Sequence and selector words are atomic: open is a
 * relaxed store followed by a release fence, so the odd value is ordered
 * before the payload; close and flip are release stores, so the payload is
 * ordered before them. Plain payload stores are only safe because the one-core
 * image never runs Root and the writer at once. */
#include "spec_writer.h"

#include <stdatomic.h>
#include <stdckdint.h>

static constexpr size_t header_size = sizeof(chryso_spec_header);
static constexpr size_t bank_size = sizeof(chryso_spec_bank);
static constexpr size_t generation_at = offsetof(chryso_spec_bank, generation);
static constexpr size_t length_at = offsetof(chryso_spec_bank, length);
static constexpr size_t crc_at = offsetof(chryso_spec_bank, crc32);
static constexpr size_t count_at = offsetof(chryso_spec_bank, record_count);
static constexpr size_t seq_at = offsetof(chryso_spec_bank, bank_seq);
static constexpr size_t budget_at = offsetof(chryso_spec_bank, budget);
static constexpr size_t desired_at = offsetof(chryso_spec_bank, desired);
static constexpr size_t tail_at = offsetof(chryso_spec_bank, reserved_tail);

static_assert(crc_at + sizeof(uint32_t) == count_at &&
                  count_at + sizeof(uint32_t) == seq_at &&
                  seq_at + sizeof(uint32_t) == budget_at,
              "the seal skips exactly crc32 and bank_seq");

static void store_u32le(uint8_t *at, uint32_t value) {
  for (size_t i = 0; i < sizeof(value); i++) {
    at[i] = (uint8_t)(value >> (8 * i));
  }
}

static void store_u64le(uint8_t *at, uint64_t value) {
  for (size_t i = 0; i < sizeof(value); i++) {
    at[i] = (uint8_t)(value >> (8 * i));
  }
}

static uint8_t *bank_bytes(chryso_spec_page *page, uint32_t bank) {
  return (uint8_t *)page + header_size + (size_t)bank * bank_size;
}

void spec_writer_init(spec_writer *writer) {
  *writer = (spec_writer){.phase = spec_writer_phase_idle};
}

spec_write_result
spec_writer_begin(spec_writer *writer, chryso_spec_page *page,
                  uint64_t generation,
                  const uint32_t budget[static chryso_child_count],
                  const uint8_t desired[static chryso_child_count]) {
  if (writer->phase != spec_writer_phase_idle &&
      writer->phase != spec_writer_phase_flipped) {
    return spec_write_bad_state;
  }
  if (page == nullptr || (uintptr_t)page % alignof(chryso_spec_page) != 0) {
    return spec_write_bad_page;
  }
  const uint8_t *header = (const uint8_t *)page;
  const bool fresh = chryso_abi_all_zero(header, header_size);
  if (!fresh &&
      chryso_check_spec_header(header, sizeof(*page)) != chryso_abi_reject_ok) {
    return spec_write_bad_page;
  }
  const uint32_t active = fresh
                              ? 0
                              : atomic_load_explicit(&page->header.active_bank,
                                                     memory_order_relaxed);
  const uint32_t bank = 1 - active;
  /* The next odd value, then the even one after it, must both fit. */
  const uint32_t seq =
      atomic_load_explicit(&page->banks[bank].bank_seq, memory_order_relaxed);
  uint32_t open_seq = 0;
  uint32_t close_seq = 0;
  if (ckd_add(&open_seq, seq, (seq & 1u) != 0 ? 2u : 1u) ||
      ckd_add(&close_seq, open_seq, 1u)) {
    return spec_write_exhausted;
  }
  if (fresh) {
    uint8_t *bytes = (uint8_t *)page;
    store_u64le(bytes + offsetof(chryso_spec_header, magic), chryso_magic_spec);
    store_u32le(bytes + offsetof(chryso_spec_header, abi_version),
                chryso_abi_version);
    atomic_store_explicit(&page->header.active_bank, 0, memory_order_release);
  }
  *writer = (spec_writer){
      .page = page,
      .phase = spec_writer_phase_idle,
      .bank = bank,
      .open_seq = open_seq,
      .generation = generation,
  };
  for (size_t c = 0; c < chryso_child_count; c++) {
    writer->budget[c] = budget[c];
    writer->desired[c] = desired[c];
  }
  return spec_write_ok;
}

/* The protocol as data: each step runs from exactly one phase. Unlisted
 * cells would read as idle, so every step is listed; the host suite checks
 * that the steps chain idle -> flipped in order. */
typedef struct {
  spec_writer_phase from;
  spec_writer_phase to;
} spec_writer_transition;

static constexpr spec_writer_transition transitions[spec_writer_step_count] = {
    [spec_writer_step_open] = {spec_writer_phase_idle, spec_writer_phase_open},
    [spec_writer_step_write] = {spec_writer_phase_open,
                                spec_writer_phase_written},
    [spec_writer_step_seal] = {spec_writer_phase_written,
                               spec_writer_phase_sealed},
    [spec_writer_step_close] = {spec_writer_phase_sealed,
                                spec_writer_phase_closed},
    [spec_writer_step_flip] = {spec_writer_phase_closed,
                               spec_writer_phase_flipped},
};

static void write_payload(spec_writer *writer) {
  uint8_t *at = bank_bytes(writer->page, writer->bank);
  store_u64le(at + generation_at, writer->generation);
  store_u32le(at + length_at, (uint32_t)bank_size);
  store_u32le(at + count_at, chryso_child_count);
  for (size_t c = 0; c < chryso_child_count; c++) {
    store_u32le(at + budget_at + c * sizeof(uint32_t), writer->budget[c]);
    at[desired_at + c] = writer->desired[c];
  }
  for (size_t i = tail_at; i < bank_size; i++) {
    at[i] = 0;
  }
}

static void seal(spec_writer *writer) {
  uint8_t *at = bank_bytes(writer->page, writer->bank);
  uint32_t crc = UINT32_C(0xFFFFFFFF);
  crc = chryso_abi_crc32_update(crc, at, crc_at);
  crc = chryso_abi_crc32_zeros(crc, sizeof(uint32_t));
  crc = chryso_abi_crc32_update(crc, at + count_at, seq_at - count_at);
  crc = chryso_abi_crc32_zeros(crc, sizeof(uint32_t));
  crc = chryso_abi_crc32_update(crc, at + budget_at, bank_size - budget_at);
  store_u32le(at + crc_at, chryso_abi_crc32_final(crc));
}

/* The stores of one step. No default, so -Wswitch flags a new step. */
static void act(spec_writer *writer, spec_writer_step_kind step) {
  chryso_spec_bank *bank = &writer->page->banks[writer->bank];
  switch (step) {
  case spec_writer_step_open:
    atomic_store_explicit(&bank->bank_seq, writer->open_seq,
                          memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    return;
  case spec_writer_step_write:
    write_payload(writer);
    return;
  case spec_writer_step_seal:
    seal(writer);
    return;
  case spec_writer_step_close:
    atomic_store_explicit(&bank->bank_seq, writer->open_seq + 1u,
                          memory_order_release);
    return;
  case spec_writer_step_flip:
    atomic_store_explicit(&writer->page->header.active_bank, writer->bank,
                          memory_order_release);
    return;
  }
}

spec_write_result spec_writer_step(spec_writer *writer,
                                   spec_writer_step_kind step) {
  if (step >= spec_writer_step_count || writer->page == nullptr ||
      writer->phase != transitions[step].from) {
    return spec_write_bad_state;
  }
  act(writer, step);
  writer->phase = transitions[step].to;
  return spec_write_ok;
}

spec_write_result
spec_writer_commit(spec_writer *writer, chryso_spec_page *page,
                   uint64_t generation,
                   const uint32_t budget[static chryso_child_count],
                   const uint8_t desired[static chryso_child_count]) {
  spec_write_result result =
      spec_writer_begin(writer, page, generation, budget, desired);
  for (size_t step = 0;
       step < spec_writer_step_count && result == spec_write_ok; step++) {
    result = spec_writer_step(writer, (spec_writer_step_kind)step);
  }
  return result;
}
