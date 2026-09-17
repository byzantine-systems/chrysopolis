/* beam_snapshot_codec.h: survivor encoding, bounded decoding, snapshot
 * validation and header predicates. */
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
         beam_survivor_read(area, used, SIZE_MAX, &cursor, &run, &payload) ==
             beam_survivor_record) {
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
    while (beam_survivor_read(area, used, len, &cursor, &run, &payload) ==
           beam_survivor_record) {
      memcpy(image + run.offset, payload, run.len);
    }
    CHECK_EQ_U64(cursor, used);
    CHECK(memcmp(image, original, len) == 0);
  }
}

static void put_record(uint8_t area[], size_t at, uint32_t offset,
                       uint32_t len) {
  const beam_survivor_hdr_t hdr = {.offset = offset, .len = len};
  memcpy(area + at, &hdr, sizeof(hdr));
}

typedef struct {
  size_t cursor;
  beam_survivor_hdr_t run;
  const uint8_t *payload;
} read_state;

/* A non-record result must leave every output exactly as it was. */
static bool read_untouched(const read_state *before, const read_state *after) {
  return before->cursor == after->cursor &&
         before->run.offset == after->run.offset &&
         before->run.len == after->run.len && before->payload == after->payload;
}

static beam_survivor_read_status read_at(const uint8_t area[], size_t len,
                                         size_t bss_len, read_state *state) {
  return beam_survivor_read(area, len, bss_len, &state->cursor, &state->run,
                            &state->payload);
}

static void test_read_bounds(void) {
  uint8_t area[40] = {};
  put_record(area, 0, 0, 8);
  const uint8_t sentinel = 0;
  const read_state start = {
      .cursor = 0, .run = {.offset = 77, .len = 77}, .payload = &sentinel};

  /* Empty area, and cursor exactly at the end. */
  read_state state = start;
  CHECK(read_at(area, 0, 64, &state) == beam_survivor_end);
  CHECK(read_untouched(&start, &state));

  /* Fewer than a header's bytes, and a payload one byte short. */
  CHECK(read_at(area, 7, 64, &state) == beam_survivor_truncated);
  CHECK(read_untouched(&start, &state));
  CHECK(read_at(area, 15, 64, &state) == beam_survivor_truncated);
  CHECK(read_untouched(&start, &state));

  /* Exactly one record, then the end. */
  CHECK(read_at(area, 16, 64, &state) == beam_survivor_record);
  CHECK_EQ_U64(state.cursor, 16);
  CHECK(state.payload == area + 8);
  CHECK_EQ_U64(state.run.len, 8);
  read_state after_record = state;
  CHECK(read_at(area, 16, 64, &state) == beam_survivor_end);
  CHECK(read_untouched(&after_record, &state));
  CHECK(read_at(area, 23, 64, &state) == beam_survivor_truncated);
  CHECK(read_untouched(&after_record, &state));

  /* A cursor past the end, including the largest one, stops instead of
   * wrapping. */
  state = start;
  state.cursor = 41;
  read_state past = state;
  CHECK(read_at(area, 40, 64, &state) == beam_survivor_truncated);
  CHECK(read_untouched(&past, &state));
  state.cursor = SIZE_MAX;
  past = state;
  CHECK(read_at(area, 40, 64, &state) == beam_survivor_truncated);
  CHECK(read_untouched(&past, &state));

  /* A payload length that would wrap the cursor arithmetic. */
  put_record(area, 0, 0, UINT32_MAX);
  state = start;
  CHECK(read_at(area, 40, SIZE_MAX, &state) == beam_survivor_truncated);
  CHECK(read_untouched(&start, &state));
}

static void test_read_bss_bound(void) {
  uint8_t area[32] = {};
  const read_state start = {};

  /* Ending exactly at the end of .bss is fine, one byte past is not. */
  put_record(area, 0, 56, 8);
  read_state state = start;
  CHECK(read_at(area, 16, 64, &state) == beam_survivor_record);
  state = start;
  CHECK(read_at(area, 16, 63, &state) == beam_survivor_out_of_bounds);
  CHECK(read_untouched(&start, &state));

  /* An offset near UINT32_MAX plus its length cannot wrap past the bound. */
  put_record(area, 0, UINT32_MAX, 8);
  state = start;
  CHECK(read_at(area, 16, UINT32_MAX, &state) == beam_survivor_out_of_bounds);
  CHECK(read_at(area, 16, (size_t)UINT32_MAX + 8, &state) ==
        beam_survivor_record);
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

/* A synthetic image: the writable segment at seg_base, file-backed data of
 * data_len bytes, then a .bss whose bytes are the caller's buffer. The
 * snapshot region sits well above it. */
enum : size_t { data_len = 0x40 };
static constexpr uintptr_t seg_base = 0x400000;
static constexpr uintptr_t snap_base = 0x30000000;
static constexpr size_t snap_size = 0x80000;
static constexpr uintptr_t cold_sp = 0xffffffe000;

static beam_restart_layout layout_for(size_t bss_len) {
  return (beam_restart_layout){
      .snapshot_base = snap_base,
      .snapshot_size = snap_size,
      .data_off = 0x6000,
      .segment_start = seg_base,
      .bss_start = seg_base + data_len,
      .bss_end = seg_base + data_len + bss_len,
      .exit_fault_base = 0xbe9ff000,
      .exit_fault_size = 0x1000,
  };
}

static beam_snapshot_hdr_t header_for(size_t survivor_bytes) {
  return (beam_snapshot_hdr_t){
      .magic = beam_snapshot_magic,
      .generation = 1,
      .saved_sp = cold_sp,
      .data_bytes = data_len,
      .survivor_bytes = survivor_bytes,
  };
}

/* Everything the encoder produces validates, and a validated area replays
 * to the original image. */
static void test_validate_accepts_encoded(void) {
  static const size_t lengths[] = {0, 8, 12, bss_bytes, bss_bytes + 5};
  for (size_t n = 0; n < sizeof(lengths) / sizeof(lengths[0]); n++) {
    const size_t len = lengths[n];
    uint8_t bss[bss_bytes + 8] = {};
    /* Sparse enough that one record per non-zero word fits the area. */
    for (size_t i = 0; i < len; i += 61) {
      bss[i] = (uint8_t)(i + 1);
    }
    if (len > 0) {
      bss[len - 1] = 0xfe;
    }
    uint8_t area[area_bytes] = {};
    size_t used = 0;
    CHECK(beam_survivors_encode(bss, len, 0, area, area_bytes, &used) ==
          beam_snapshot_ok);

    const beam_restart_layout layout = layout_for(len);
    CHECK(beam_restart_layout_check(&layout) == beam_restart_layout_ok);
    const beam_snapshot_hdr_t hdr = header_for(used);
    CHECK(beam_snapshot_validate(&hdr, area, area_bytes, &layout) ==
          beam_snapshot_ok);

    uint8_t image[bss_bytes + 8] = {};
    size_t cursor = 0;
    beam_survivor_hdr_t run = {};
    const uint8_t *payload = nullptr;
    beam_survivor_read_status status = beam_survivor_end;
    while ((status = beam_survivor_read(area, used, len, &cursor, &run,
                                        &payload)) == beam_survivor_record) {
      memcpy(image + run.offset, payload, run.len);
    }
    CHECK(status == beam_survivor_end);
    CHECK(memcmp(image, bss, len) == 0);
  }
}

static void test_validate_header(void) {
  const beam_restart_layout layout = layout_for(bss_bytes);
  const uint8_t area[16] = {};

  beam_snapshot_hdr_t hdr = header_for(0);
  CHECK(beam_snapshot_validate(&hdr, area, 0, &layout) == beam_snapshot_ok);

  hdr.magic ^= 1;
  CHECK(beam_snapshot_validate(&hdr, area, 0, &layout) ==
        beam_snapshot_not_captured);

  hdr = header_for(0);
  hdr.generation = 0;
  CHECK(beam_snapshot_validate(&hdr, area, 0, &layout) ==
        beam_snapshot_bad_generation);
  hdr.generation = UINT64_MAX;
  CHECK(beam_snapshot_validate(&hdr, area, 0, &layout) ==
        beam_snapshot_bad_generation);
  hdr.generation = UINT64_MAX - 1;
  CHECK(beam_snapshot_validate(&hdr, area, 0, &layout) == beam_snapshot_ok);

  /* The data length must match exactly: shorter would leave stale bytes,
   * longer would restore past .data into .bss. */
  hdr = header_for(0);
  hdr.data_bytes = data_len - 1;
  CHECK(beam_snapshot_validate(&hdr, area, 0, &layout) ==
        beam_snapshot_data_length_mismatch);
  hdr.data_bytes = data_len + 1;
  CHECK(beam_snapshot_validate(&hdr, area, 0, &layout) ==
        beam_snapshot_data_length_mismatch);
  hdr.data_bytes = UINT64_MAX;
  CHECK(beam_snapshot_validate(&hdr, area, 0, &layout) ==
        beam_snapshot_data_length_mismatch);

  hdr = header_for(sizeof(area));
  CHECK(beam_snapshot_validate(&hdr, area, sizeof(area) - 1, &layout) ==
        beam_snapshot_survivor_length_too_large);
  hdr.survivor_bytes = UINT64_MAX;
  CHECK(beam_snapshot_validate(&hdr, area, sizeof(area), &layout) ==
        beam_snapshot_survivor_length_too_large);
}

static void test_validate_stack_pointer(void) {
  const beam_restart_layout layout = layout_for(bss_bytes);
  const uint8_t area[1] = {};
  beam_snapshot_hdr_t hdr = header_for(0);

  static const uint64_t bad[] = {
      0,
      cold_sp + 8,
      cold_sp + 1,
      /* The top of a stack inside the snapshot region, including its end. */
      snap_base + 16,
      snap_base + snap_size,
      /* Inside the writable segment, at both of its edges. */
      seg_base + 16,
      seg_base + data_len + bss_bytes,
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    hdr.saved_sp = bad[i];
    CHECK(beam_snapshot_validate(&hdr, area, 0, &layout) ==
          beam_snapshot_bad_stack_pointer);
  }

  /* Tops exactly at the start of a region grow away from it. */
  static const uint64_t good[] = {
      snap_base,
      seg_base,
      /* The first aligned top past each region's end. */
      (snap_base + snap_size + 16) & ~UINT64_C(15),
      (seg_base + data_len + bss_bytes + 16) & ~UINT64_C(15),
      UINT64_MAX & ~UINT64_C(15),
  };
  for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
    hdr.saved_sp = good[i];
    CHECK(beam_snapshot_validate(&hdr, area, 0, &layout) == beam_snapshot_ok);
  }
}

/* Validate a hand-built survivor area for a .bss of bss_len bytes. */
static beam_snapshot_status validate_area(const uint8_t area[], size_t used,
                                          size_t bss_len) {
  const beam_restart_layout layout = layout_for(bss_len);
  const beam_snapshot_hdr_t hdr = header_for(used);
  return beam_snapshot_validate(&hdr, area, area_bytes, &layout);
}

static void test_validate_records(void) {
  uint8_t area[area_bytes] = {};

  /* Two ordered word runs, touching, then the sub-word tail. */
  put_record(area, 0, 0, 8);
  put_record(area, 16, 8, 16);
  put_record(area, 40, 24, 4);
  CHECK(validate_area(area, 52, 28) == beam_snapshot_ok);

  /* Truncated: the used length cuts into a payload or a header. */
  CHECK(validate_area(area, 51, 28) == beam_snapshot_survivor_truncated);
  CHECK(validate_area(area, 44, 28) == beam_snapshot_survivor_truncated);

  /* Out of bounds: .bss one byte shorter than the tail's end. */
  CHECK(validate_area(area, 52, 27) == beam_snapshot_survivor_out_of_bounds);

  /* The tail rule applies only at the real tail offset and end. */
  CHECK(validate_area(area, 52, 36) == beam_snapshot_survivor_misaligned);
  put_record(area, 40, 24, 3);
  CHECK(validate_area(area, 51, 28) == beam_snapshot_survivor_misaligned);
  CHECK(validate_area(area, 51, 27) == beam_snapshot_ok);

  /* A word run with a ragged length, and a misaligned offset. */
  memset(area, 0, sizeof(area));
  put_record(area, 0, 0, 9);
  CHECK(validate_area(area, 17, 64) == beam_snapshot_survivor_misaligned);
  put_record(area, 0, 4, 8);
  CHECK(validate_area(area, 16, 64) == beam_snapshot_survivor_misaligned);

  /* Empty records. */
  put_record(area, 0, 8, 0);
  CHECK(validate_area(area, 8, 64) == beam_snapshot_survivor_empty);

  /* Overlapping by one word, and a record repeated. */
  memset(area, 0, sizeof(area));
  put_record(area, 0, 8, 16);
  put_record(area, 24, 16, 8);
  CHECK(validate_area(area, 40, 64) == beam_snapshot_survivor_out_of_order);
  put_record(area, 24, 8, 16);
  CHECK(validate_area(area, 48, 64) == beam_snapshot_survivor_out_of_order);
  put_record(area, 24, 0, 8);
  CHECK(validate_area(area, 40, 64) == beam_snapshot_survivor_out_of_order);
  put_record(area, 24, 24, 8);
  CHECK(validate_area(area, 40, 64) == beam_snapshot_ok);

  /* A one-byte tail recorded twice: the repeat starts one byte before the
   * previous end, the smallest overlap alignment allows. */
  memset(area, 0, sizeof(area));
  put_record(area, 0, 24, 1);
  CHECK(validate_area(area, 9, 25) == beam_snapshot_ok);
  put_record(area, 9, 24, 1);
  CHECK(validate_area(area, 18, 25) == beam_snapshot_survivor_out_of_order);
}

static void test_status_names(void) {
  static const beam_snapshot_status all[] = {
      beam_snapshot_ok,
      beam_snapshot_survivor_overflow,
      beam_snapshot_bss_too_large,
      beam_snapshot_not_captured,
      beam_snapshot_bad_generation,
      beam_snapshot_data_length_mismatch,
      beam_snapshot_survivor_length_too_large,
      beam_snapshot_bad_stack_pointer,
      beam_snapshot_survivor_truncated,
      beam_snapshot_survivor_out_of_bounds,
      beam_snapshot_survivor_empty,
      beam_snapshot_survivor_misaligned,
      beam_snapshot_survivor_out_of_order,
  };
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
    const char *name = beam_snapshot_status_name(all[i]);
    CHECK(name[0] != '\0');
    CHECK(strcmp(name, "unknown") != 0);
    for (size_t j = 0; j < i; j++) {
      CHECK(strcmp(name, beam_snapshot_status_name(all[j])) != 0);
    }
  }
}

int main(void) {
  test_all_zero();
  test_single_word();
  test_gap_coalescing();
  test_sub_word_tail();
  test_capacity_boundary();
  test_bss_too_large();
  test_round_trip_cycles();
  test_read_bounds();
  test_read_bss_bound();
  test_validate_accepts_encoded();
  test_validate_header();
  test_validate_stack_pointer();
  test_validate_records();
  test_status_names();
  test_header_predicates();
  test_exit_fault_addr();
  return check_finish("snapshot");
}
