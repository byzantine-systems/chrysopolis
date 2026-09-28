/* Generated orchestration headers: guards, layout against hand-written
 * literals, CRC-32, the status-copy decision and checkers on unaligned bytes.
 * The rule-by-rule verdicts are compared with the Zig reference in
 * abi_differential.zig. */
#include "check.h"

#include <chrysopolis/orchestrator_abi.h>
#include <chrysopolis/root_control.h>
#include <chrysopolis/system_abi.h>
#include <chrysopolis/worker_identity.h>
#include <chrysopolis/worker_protocol.h>
#include <chrysopolis/worker_status.h>

/* Second inclusions go through macros so include sorting keeps them; each
 * must be a no-op behind its guard. */
#define ABI_HEADER_ORCHESTRATOR <chrysopolis/orchestrator_abi.h>
#define ABI_HEADER_ROOT_CONTROL <chrysopolis/root_control.h>
#define ABI_HEADER_SYSTEM <chrysopolis/system_abi.h>
#define ABI_HEADER_IDENTITY <chrysopolis/worker_identity.h>
#define ABI_HEADER_PROTOCOL <chrysopolis/worker_protocol.h>
#define ABI_HEADER_STATUS <chrysopolis/worker_status.h>
#include ABI_HEADER_ORCHESTRATOR
#include ABI_HEADER_ROOT_CONTROL
#include ABI_HEADER_SYSTEM
#include ABI_HEADER_IDENTITY
#include ABI_HEADER_PROTOCOL
#include ABI_HEADER_STATUS

#include <string.h>

/* Literals from the wire specification, not from the generator. */
static_assert(sizeof(chryso_root_status_header) == 88);
static_assert(sizeof(chryso_root_child_status) == 80);
static_assert(sizeof(chryso_root_event) == 32);
static_assert(sizeof(chryso_root_status_page) == 16384);
static_assert(offsetof(chryso_root_status_page, events) == 5048);
static_assert(offsetof(chryso_root_status_header, applied_spec_generation) ==
              72);
static_assert(offsetof(chryso_root_status_page, reserved_tail) == 9144);
static_assert(offsetof(chryso_root_status_header, seq) == 16);
static_assert(sizeof(chryso_spec_header) == 64);
static_assert(sizeof(chryso_spec_bank) == 2016);
static_assert(offsetof(chryso_spec_page, banks) == 64);
static_assert(offsetof(chryso_spec_page, banks[1]) == 2080);
static_assert(offsetof(chryso_spec_bank, bank_seq) == 20);
static_assert(offsetof(chryso_spec_bank, reserved_tail) == 334);
static_assert(sizeof(chryso_spec_page) == 4096);
static_assert(sizeof(chryso_ctl_command) == 64);
static_assert(sizeof(chryso_ctl_reply) == 64);
static_assert(sizeof(chryso_worker_identity) == 4096);
static_assert(offsetof(chryso_worker_status_header, status_seq) == 80);
static_assert(sizeof(chryso_worker_status) == 4096);
static_assert(sizeof(chryso_journal_header) == 256);
static_assert(sizeof(chryso_journal_entry) == 256);
static_assert(sizeof(chryso_journal_page) == 4096);
static_assert(sizeof(chryso_completion_kind) == 4);
static_assert(sizeof(chryso_pp_opcode) == 2);
static_assert(sizeof(chryso_root_event_kind) == 1);

static void put_u16(uint8_t *at, uint16_t value) {
  at[0] = (uint8_t)value;
  at[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *at, uint32_t value) {
  for (size_t i = 0; i < 4; i++) {
    at[i] = (uint8_t)(value >> (8 * i));
  }
}

static void put_u64(uint8_t *at, uint64_t value) {
  for (size_t i = 0; i < 8; i++) {
    at[i] = (uint8_t)(value >> (8 * i));
  }
}

static void test_crc32(void) {
  /* The CRC-32/IEEE catalogue check value. */
  static const uint8_t check_input[] = "123456789";
  CHECK_EQ_U64(chryso_abi_crc32(check_input, sizeof check_input - 1),
               UINT32_C(0xCBF43926));
  CHECK_EQ_U64(chryso_abi_crc32(nullptr, 0), 0);
  /* Feeding zeros equals hashing a zero buffer. */
  static const uint8_t zeros[4] = {};
  CHECK_EQ_U64(chryso_abi_crc32_zeros(UINT32_C(0xFFFFFFFF), 4),
               chryso_abi_crc32_update(UINT32_C(0xFFFFFFFF), zeros, 4));
}

static void test_magic_bytes(void) {
  uint8_t bytes[8] = {};
  put_u64(bytes, chryso_magic_status);
  CHECK(memcmp(bytes, "CHRYSTA1", 8) == 0);
  put_u64(bytes, chryso_magic_completion);
  CHECK(memcmp(bytes, "CHRYCMP1", 8) == 0);
}

static void test_status_copy_verdict(void) {
  CHECK_EQ_U64(chryso_status_copy_verdict(2, 2), chryso_abi_reject_ok);
  CHECK_EQ_U64(chryso_status_copy_verdict(0, 0), chryso_abi_reject_torn);
  CHECK_EQ_U64(chryso_status_copy_verdict(3, 3), chryso_abi_reject_torn);
  CHECK_EQ_U64(chryso_status_copy_verdict(2, 4), chryso_abi_reject_torn);
  CHECK_EQ_U64(chryso_status_copy_verdict(4, 2), chryso_abi_reject_torn);
  CHECK_EQ_U64(chryso_status_copy_verdict(UINT64_MAX - 1, UINT64_MAX - 1),
               chryso_abi_reject_ok);
}

/* One byte of slack so every checker also runs on an odd address. */
static uint8_t page[sizeof(chryso_root_status_page) + 1];

static uint8_t *ctl_command_at(size_t shift) {
  uint8_t *const at = page + shift;
  memset(page, 0, sizeof(page));
  put_u64(at, chryso_magic_command);
  put_u16(at + offsetof(chryso_ctl_command, version),
          (uint16_t)chryso_abi_version);
  put_u16(at + offsetof(chryso_ctl_command, opcode), chryso_pp_opcode_sync);
  return at;
}

static uint8_t *spec_at(size_t shift) {
  uint8_t *const at = page + shift;
  uint8_t *const bank = at + offsetof(chryso_spec_page, banks);
  memset(page, 0, sizeof(page));
  put_u64(at, chryso_magic_spec);
  put_u32(at + offsetof(chryso_spec_header, abi_version), chryso_abi_version);
  put_u32(bank + offsetof(chryso_spec_bank, length), sizeof(chryso_spec_bank));
  put_u32(bank + offsetof(chryso_spec_bank, record_count), chryso_child_count);
  /* Seal with the sequence still zero, then publish it. */
  put_u32(bank + offsetof(chryso_spec_bank, crc32), 0);
  put_u32(bank + offsetof(chryso_spec_bank, crc32),
          chryso_abi_crc32(bank, sizeof(chryso_spec_bank)));
  put_u32(bank + offsetof(chryso_spec_bank, bank_seq), 2);
  return at;
}

static void test_unaligned_checkers(void) {
  for (size_t shift = 0; shift < 2; shift++) {
    constexpr size_t ctl_size = sizeof(chryso_ctl_command);
    uint8_t *at = ctl_command_at(shift);
    CHECK_EQ_U64(chryso_check_ctl_command(at, ctl_size), chryso_abi_reject_ok);
    CHECK_EQ_U64(chryso_check_ctl_command(at, ctl_size - 1),
                 chryso_abi_reject_size);
    CHECK_EQ_U64(chryso_check_ctl_command(nullptr, ctl_size),
                 chryso_abi_reject_size);
    at[offsetof(chryso_ctl_command, reserved)] = 1;
    CHECK_EQ_U64(chryso_check_ctl_command(at, ctl_size),
                 chryso_abi_reject_reserved);

    constexpr size_t spec_size = sizeof(chryso_spec_page);
    at = spec_at(shift);
    CHECK_EQ_U64(chryso_check_spec_header(at, spec_size), chryso_abi_reject_ok);
    CHECK_EQ_U64(chryso_check_spec_bank(at, spec_size, 0),
                 chryso_abi_reject_ok);
    CHECK_EQ_U64(chryso_check_spec_bank(at, spec_size, 1),
                 chryso_abi_reject_torn);
    CHECK_EQ_U64(chryso_check_spec_bank(at, spec_size, 2),
                 chryso_abi_reject_range);
    /* A stored CRC one off is an integrity failure, even with a dirty tail. */
    uint8_t *const bank = at + offsetof(chryso_spec_page, banks);
    uint8_t *const crc = bank + offsetof(chryso_spec_bank, crc32);
    put_u32(crc, chryso_abi_load_u32le(crc) ^ 1u);
    bank[offsetof(chryso_spec_bank, reserved_tail)] = 1;
    CHECK_EQ_U64(chryso_check_spec_bank(at, spec_size, 0),
                 chryso_abi_reject_checksum);
  }
}

static void test_membership(void) {
  CHECK(chryso_completion_kind_is_member(chryso_completion_kind_work));
  CHECK(!chryso_completion_kind_is_member(0x8000u));
  CHECK(chryso_completion_status_is_member(chryso_completion_status_ok));
  CHECK(!chryso_root_event_kind_is_member(10));
  CHECK(chryso_slot_transitions[0].from == chryso_slot_phase_unassigned);
}

static void test_topology(void) {
  CHECK_EQ_U64(chryso_control_status_size, sizeof(chryso_root_status_page));
  CHECK_EQ_U64(chryso_control_spec_size, sizeof(chryso_spec_page));
  CHECK_EQ_U64(chryso_pool_identity_size, sizeof(chryso_worker_identity));
  unsigned int slots = 0;
  for (size_t i = 0; i < chryso_pool_classes_count; i++) {
    slots += chryso_pool_classes[i].count;
  }
  CHECK_EQ_U64(slots, chryso_pool_slots);
}

int main(void) {
  test_crc32();
  test_magic_bytes();
  test_status_copy_verdict();
  test_unaligned_checkers();
  test_membership();
  test_topology();
  return check_finish("abi_layout");
}
