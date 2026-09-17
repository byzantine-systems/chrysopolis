/* beam_snapshot_codec.h: survivor encoding, decoding and snapshot header
 * predicates. */
#include "check.h"

#include "beam_snapshot_codec.h"

#include <stdint.h>
#include <string.h>

enum : size_t { bss_words = 64, bss_bytes = bss_words * 8, area_bytes = 512 };

static constexpr size_t gap = 8;

static void put_word(uint8_t bss[], size_t index, uint64_t value) {
  memcpy(bss + index * 8, &value, sizeof(value));
}

typedef struct {
  size_t count;
  beam_survivor_hdr_t runs[16];
} decoded;

static decoded decode(const uint8_t area[], size_t used) {
  decoded result = {};
  size_t cursor = 0;
  beam_survivor_hdr_t run = {};
  const uint8_t *payload = nullptr;
  while (result.count < 16 &&
         beam_survivor_next(area, used, &cursor, &run, &payload)) {
    result.runs[result.count++] = run;
  }
  return result;
}

static void test_all_zero(void) {
  uint8_t bss[bss_bytes] = {};
  uint8_t area[area_bytes] = {};
  size_t used = SIZE_MAX;
  CHECK(beam_survivors_encode(bss, bss_bytes, gap, area, area_bytes, &used) ==
        beam_snapshot_ok);
  CHECK_EQ_U64(used, 0);
  CHECK(beam_survivors_encode(nullptr, 0, gap, area, area_bytes, &used) ==
        beam_snapshot_ok);
  CHECK_EQ_U64(used, 0);
}

static void test_single_word(void) {
  uint8_t bss[bss_bytes] = {};
  put_word(bss, 5, 0x1122334455667788u);
  uint8_t area[area_bytes] = {};
  size_t used = 0;
  CHECK(beam_survivors_encode(bss, bss_bytes, gap, area, area_bytes, &used) ==
        beam_snapshot_ok);
  CHECK_EQ_U64(used, 16);
  const decoded d = decode(area, used);
  CHECK_EQ_U64(d.count, 1);
  CHECK_EQ_U64(d.runs[0].offset, 40);
  CHECK_EQ_U64(d.runs[0].len, 8);
  CHECK(memcmp(area + 8, bss + 40, 8) == 0);
}

/* Zero words at most gap apart stay inside one run; one more splits it. */
static void test_gap_coalescing(void) {
  uint8_t bss[bss_bytes] = {};
  put_word(bss, 0, 1);
  put_word(bss, gap, 2);
  uint8_t area[area_bytes] = {};
  size_t used = 0;
  CHECK(beam_survivors_encode(bss, bss_bytes, gap, area, area_bytes, &used) ==
        beam_snapshot_ok);
  decoded d = decode(area, used);
  CHECK_EQ_U64(d.count, 1);
  CHECK_EQ_U64(d.runs[0].offset, 0);
  CHECK_EQ_U64(d.runs[0].len, (gap + 1) * 8);

  memset(bss, 0, sizeof(bss));
  put_word(bss, 0, 1);
  put_word(bss, gap + 1, 2);
  CHECK(beam_survivors_encode(bss, bss_bytes, gap, area, area_bytes, &used) ==
        beam_snapshot_ok);
  d = decode(area, used);
  CHECK_EQ_U64(d.count, 2);
  CHECK_EQ_U64(d.runs[0].len, 8);
  CHECK_EQ_U64(d.runs[1].offset, (gap + 1) * 8);

  /* A zero gap never coalesces, even adjacent words. */
  memset(bss, 0, sizeof(bss));
  put_word(bss, 3, 1);
  put_word(bss, 4, 1);
  CHECK(beam_survivors_encode(bss, bss_bytes, 0, area, area_bytes, &used) ==
        beam_snapshot_ok);
  CHECK_EQ_U64(decode(area, used).count, 2);
}

/* A .bss length that is not a word multiple keeps its tail bytes. */
static void test_sub_word_tail(void) {
  uint8_t bss[bss_bytes] = {};
  uint8_t area[area_bytes] = {};
  size_t used = 0;
  static constexpr size_t len = 8 * 3 + 4;

  CHECK(beam_survivors_encode(bss, len, gap, area, area_bytes, &used) ==
        beam_snapshot_ok);
  CHECK_EQ_U64(used, 0);

  bss[len - 1] = 0xab;
  /* A byte past len must never be recorded. */
  bss[len] = 0xcd;
  CHECK(beam_survivors_encode(bss, len, gap, area, area_bytes, &used) ==
        beam_snapshot_ok);
  const decoded d = decode(area, used);
  CHECK_EQ_U64(d.count, 1);
  CHECK_EQ_U64(d.runs[0].offset, 24);
  CHECK_EQ_U64(d.runs[0].len, 4);
  CHECK(area[8 + 3] == 0xab);
  CHECK_EQ_U64(used, 12);
}

static void test_capacity_boundary(void) {
  uint8_t bss[bss_bytes] = {};
  put_word(bss, 0, 1);
  put_word(bss, 20, 1);
  uint8_t area[area_bytes] = {};
  size_t used = SIZE_MAX;

  /* Two records of 16 bytes: exactly 32 fits, 31 and a header-only 8 do not. */
  CHECK(beam_survivors_encode(bss, bss_bytes, gap, area, 32, &used) ==
        beam_snapshot_ok);
  CHECK_EQ_U64(used, 32);
  used = SIZE_MAX;
  CHECK(beam_survivors_encode(bss, bss_bytes, gap, area, 31, &used) ==
        beam_snapshot_survivor_overflow);
  CHECK(beam_survivors_encode(bss, bss_bytes, gap, area, 8, &used) ==
        beam_snapshot_survivor_overflow);
  CHECK(beam_survivors_encode(bss, bss_bytes, gap, area, 0, &used) ==
        beam_snapshot_survivor_overflow);
  CHECK_EQ_U64(used, SIZE_MAX);

  /* The tail record is bounded the same way. */
  uint8_t tail_bss[12] = {};
  tail_bss[10] = 1;
  CHECK(beam_survivors_encode(tail_bss, sizeof(tail_bss), gap, area, 11,
                              &used) == beam_snapshot_survivor_overflow);
  CHECK(beam_survivors_encode(tail_bss, sizeof(tail_bss), gap, area, 12,
                              &used) == beam_snapshot_ok);
}

static void test_bss_too_large(void) {
  uint8_t sentinel = 0;
  uint8_t area[area_bytes] = {};
  size_t used = 7;
  /* Rejected before any byte is read. */
  CHECK(beam_survivors_encode(&sentinel, (size_t)UINT32_MAX + 1, gap, area,
                              area_bytes,
                              &used) == beam_snapshot_bss_too_large);
  CHECK_EQ_U64(used, 7);
}

/* Encode, zero .bss as the reset does, replay the survivors: the image must
 * come back exactly, cycle after cycle. */
static void test_round_trip_cycles(void) {
  uint8_t original[bss_bytes + 5] = {};
  put_word(original, 0, 0xdead);
  put_word(original, 1, 0xbeef);
  put_word(original, 7, UINT64_MAX);
  put_word(original, 30, 42);
  put_word(original, 63, UINT64_C(1) << 63);
  original[bss_bytes + 2] = 9;
  const size_t len = sizeof(original);

  uint8_t area[area_bytes] = {};
  size_t used = 0;
  CHECK(beam_survivors_encode(original, len, gap, area, area_bytes, &used) ==
        beam_snapshot_ok);

  uint8_t image[sizeof(original)] = {};
  memcpy(image, original, len);
  for (size_t cycle = 0; cycle < 4; cycle++) {
    /* Scribble over the image as a crashed run would, then reset it. */
    memset(image, 0x5a, len);
    memset(image, 0, len);
    size_t cursor = 0;
    beam_survivor_hdr_t run = {};
    const uint8_t *payload = nullptr;
    while (beam_survivor_next(area, used, &cursor, &run, &payload)) {
      memcpy(image + run.offset, payload, run.len);
    }
    CHECK_EQ_U64(cursor, used);
    CHECK(memcmp(image, original, len) == 0);
  }
}

static void test_iterator_bounds(void) {
  uint8_t area[24] = {};
  const beam_survivor_hdr_t hdr = {.offset = 0, .len = 8};
  memcpy(area, &hdr, sizeof(hdr));

  size_t cursor = 0;
  beam_survivor_hdr_t run = {};
  const uint8_t *payload = nullptr;
  CHECK(!beam_survivor_next(area, 0, &cursor, &run, &payload));
  CHECK(!beam_survivor_next(area, 7, &cursor, &run, &payload));
  CHECK_EQ_U64(cursor, 0);
  CHECK(payload == nullptr);

  CHECK(beam_survivor_next(area, 16, &cursor, &run, &payload));
  CHECK_EQ_U64(cursor, 16);
  CHECK(payload == area + 8);
  CHECK(!beam_survivor_next(area, 16, &cursor, &run, &payload));
  CHECK(!beam_survivor_next(area, 23, &cursor, &run, &payload));

  /* A cursor already past the end stops instead of wrapping. */
  cursor = 40;
  CHECK(!beam_survivor_next(area, 24, &cursor, &run, &payload));
  CHECK_EQ_U64(cursor, 40);
}

static void test_header_predicates(void) {
  CHECK(beam_snapshot_captured(beam_snapshot_magic));
  CHECK(!beam_snapshot_captured(0));
  CHECK(!beam_snapshot_captured(beam_snapshot_magic ^ 1));
  /* The magic spells CHRYSNP1 in big-endian byte order. */
  CHECK_EQ_U64(beam_snapshot_magic, 0x43485259534E5031u);

  CHECK(!beam_snapshot_warm(beam_snapshot_magic, 0));
  CHECK(!beam_snapshot_warm(beam_snapshot_magic, 1));
  CHECK(beam_snapshot_warm(beam_snapshot_magic, 2));
  CHECK(beam_snapshot_warm(beam_snapshot_magic, UINT64_MAX));
  CHECK(!beam_snapshot_warm(0, 5));
}

static void test_exit_fault_addr(void) {
  static constexpr uintptr_t base = 0x100000;
  CHECK(beam_exit_fault_addr(base, 0) == base);
  CHECK(beam_exit_fault_addr(base, 3) == base + 3);
  CHECK(beam_exit_fault_addr(base, 255) == base + 255);
  CHECK(beam_exit_fault_addr(base, 256) == base);
  CHECK(beam_exit_fault_addr(base, -1) == base + 255);
  CHECK(beam_exit_fault_addr(base, INT32_MIN) == base);
}

int main(void) {
  test_all_zero();
  test_single_word();
  test_gap_coalescing();
  test_sub_word_tail();
  test_capacity_boundary();
  test_bss_too_large();
  test_round_trip_cycles();
  test_iterator_bounds();
  test_header_predicates();
  test_exit_fault_addr();
  return check_finish("snapshot");
}
