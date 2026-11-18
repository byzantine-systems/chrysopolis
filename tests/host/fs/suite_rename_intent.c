/* rename_intent.h, the LionsOS fork's FAT rename record: the vector pinned
 * with tools/testbed's renaming.py, the codec's round trip, single-byte
 * corruption, totality, the volume bounds and the mount decision. */
#include "check.h"
#include "prop.h"

#include "rename_intent.h"

#include <stdint.h>
#include <string.h>

enum : size_t { big = 4096, small = 512, cases = 2000 };

/* PINNED in tools/testbed/tests/test_renaming.py */
static const uint8_t pinned[RENAME_INTENT_SIZE] = {
    0x43, 0x48, 0x52, 0x59, 0x52, 0x4e, 0x4d, 0x31, 0x01, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00,
    0x46, 0x55, 0x5a, 0x5a, 0x31, 0x20, 0x20, 0x20, 0x42, 0x49, 0x4e, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xd0, 0x62, 0xde, 0x97,
};

static struct rename_intent pinned_intent(void) {
  struct rename_intent intent = {
      .old_dir = 2, .blk_ofs = 0x40, .dptr = 0x60, .cluster = 9, .new_dir = 2};
  memcpy(intent.sfn, "FUZZ1   BIN", RENAME_INTENT_NAME);
  return intent;
}

static bool same(const struct rename_intent *a, const struct rename_intent *b) {
  return a->old_dir == b->old_dir && a->blk_ofs == b->blk_ofs &&
         a->dptr == b->dptr && a->cluster == b->cluster &&
         a->new_dir == b->new_dir &&
         memcmp(a->sfn, b->sfn, RENAME_INTENT_NAME) == 0;
}

static uint32_t word(prop_rng *rng) { return (uint32_t)prop_next(rng); }

static struct rename_intent random_intent(prop_rng *rng) {
  struct rename_intent intent = {.old_dir = word(rng),
                                 .blk_ofs = word(rng),
                                 .dptr = word(rng),
                                 .cluster = word(rng),
                                 .new_dir = word(rng)};
  for (size_t i = 0; i < RENAME_INTENT_NAME; i++) {
    intent.sfn[i] = (uint8_t)prop_next(rng);
  }
  return intent;
}

static void test_pinned_vector(void) {
  uint8_t sector[big] = {};
  struct rename_intent intent = pinned_intent();
  CHECK(rename_intent_encode(sector, big, &intent));
  CHECK(memcmp(sector, pinned, RENAME_INTENT_SIZE) == 0);
  uint8_t padded[big] = {};
  memcpy(padded, pinned, RENAME_INTENT_SIZE);
  struct rename_intent out = {};
  CHECK(rename_intent_decode(padded, big, &out) == RENAME_INTENT_VALID);
  CHECK(same(&out, &intent));
}

static void test_round_trip(void) {
  prop_rng rng = prop_seed(0x52454e414d45ULL);
  for (size_t n = 0; n < cases; n++) {
    size_t size = prop_chance(&rng, 50) ? big : small;
    uint8_t sector[big] = {};
    struct rename_intent intent = random_intent(&rng);
    CHECK(rename_intent_encode(sector, size, &intent));
    struct rename_intent out = {};
    CHECK(rename_intent_decode(sector, size, &out) == RENAME_INTENT_VALID);
    CHECK(same(&out, &intent));
  }
}

/* The complement of the round trip: a decoder that accepts everything passes
 * it, and fails this. Half the flips land in the record, or the zero tail
 * would hide a weak check. */
static void test_one_flipped_byte_is_never_valid(void) {
  prop_rng rng = prop_seed(0x464c4950ULL);
  for (size_t n = 0; n < cases; n++) {
    size_t size = prop_chance(&rng, 50) ? big : small;
    uint8_t sector[big] = {};
    struct rename_intent intent = random_intent(&rng);
    CHECK(rename_intent_encode(sector, size, &intent));
    size_t at =
        prop_chance(&rng, 50)
            ? prop_below(&rng, RENAME_INTENT_SIZE)
            : RENAME_INTENT_SIZE + prop_below(&rng, size - RENAME_INTENT_SIZE);
    sector[at] ^= (uint8_t)(1 + prop_below(&rng, 255));
    struct rename_intent out = {};
    enum rename_intent_status status = rename_intent_decode(sector, size, &out);
    CHECK(status == RENAME_INTENT_FOREIGN || status == RENAME_INTENT_CORRUPT);
  }
}

/* Any bytes decode to some status, under UBSan, and never write `out` unless
 * valid. */
static void test_decode_is_total(void) {
  prop_rng rng = prop_seed(0x544f54414cULL);
  for (size_t n = 0; n < cases; n++) {
    uint8_t sector[small] = {};
    size_t size = prop_below(&rng, small + 1);
    for (size_t i = 0; i < size; i++) {
      sector[i] = (uint8_t)prop_next(&rng);
    }
    if (prop_chance(&rng, 30) && size >= sizeof rename_intent_magic) {
      memcpy(sector, rename_intent_magic, sizeof rename_intent_magic);
    }
    struct rename_intent out = {.cluster = 0xabcdU};
    enum rename_intent_status status = rename_intent_decode(sector, size, &out);
    CHECK(status <= RENAME_INTENT_CORRUPT);
    CHECK(status == RENAME_INTENT_VALID || out.cluster == 0xabcdU);
  }
}

static void test_status_edges(void) {
  uint8_t sector[small] = {};
  struct rename_intent out = {};
  CHECK(rename_intent_decode(sector, small, &out) == RENAME_INTENT_ABSENT);
  CHECK(rename_intent_decode(sector, 0, &out) == RENAME_INTENT_ABSENT);

  struct rename_intent intent = pinned_intent();
  CHECK(rename_intent_encode(sector, small, &intent));
  sector[0] = 'X';
  CHECK(rename_intent_decode(sector, small, &out) == RENAME_INTENT_FOREIGN);

  CHECK(rename_intent_encode(sector, small, &intent));
  sector[RENAME_INTENT_AT_VERSION] = 2;
  CHECK(rename_intent_decode(sector, small, &out) == RENAME_INTENT_FOREIGN);

  CHECK(rename_intent_encode(sector, small, &intent));
  sector[small - 1] = 1;
  CHECK(rename_intent_decode(sector, small, &out) == RENAME_INTENT_CORRUPT);

  CHECK(rename_intent_encode(sector, small, &intent));
  CHECK(rename_intent_decode(sector, RENAME_INTENT_SIZE - 1, &out) ==
        RENAME_INTENT_FOREIGN);

  uint8_t tiny[RENAME_INTENT_SIZE - 1] = {};
  CHECK(!rename_intent_encode(tiny, sizeof tiny, &intent));
  CHECK(rename_intent_decode(tiny, sizeof tiny, &out) == RENAME_INTENT_ABSENT);
}

static void test_in_bounds_edges(void) {
  enum : uint32_t { clusters = 100, dir_bytes = 0x1000 };
  struct rename_intent ok = pinned_intent();
  CHECK(rename_intent_in_bounds(&ok, clusters, dir_bytes));

  struct rename_intent t = ok;
  t.cluster = 0;
  CHECK(!rename_intent_in_bounds(&t, clusters, dir_bytes));
  t.cluster = 1;
  CHECK(!rename_intent_in_bounds(&t, clusters, dir_bytes));
  t.cluster = clusters - 1;
  CHECK(rename_intent_in_bounds(&t, clusters, dir_bytes));
  t.cluster = clusters;
  CHECK(!rename_intent_in_bounds(&t, clusters, dir_bytes));

  t = ok;
  t.old_dir = 0; /* a FAT12/16 root */
  CHECK(rename_intent_in_bounds(&t, clusters, dir_bytes));
  t.old_dir = 1;
  CHECK(!rename_intent_in_bounds(&t, clusters, dir_bytes));
  t = ok;
  t.new_dir = clusters;
  CHECK(!rename_intent_in_bounds(&t, clusters, dir_bytes));

  t = ok;
  t.dptr = 0x61;
  CHECK(!rename_intent_in_bounds(&t, clusters, dir_bytes));
  t.dptr = dir_bytes - RENAME_INTENT_ENTRY;
  CHECK(rename_intent_in_bounds(&t, clusters, dir_bytes));
  t.dptr = dir_bytes;
  CHECK(!rename_intent_in_bounds(&t, clusters, dir_bytes));

  t = ok;
  t.blk_ofs = t.dptr;
  CHECK(rename_intent_in_bounds(&t, clusters, dir_bytes));
  t.blk_ofs = t.dptr + RENAME_INTENT_ENTRY;
  CHECK(!rename_intent_in_bounds(&t, clusters, dir_bytes));
  t.blk_ofs = 0x41;
  CHECK(!rename_intent_in_bounds(&t, clusters, dir_bytes));
  t.blk_ofs = RENAME_INTENT_NO_LFN;
  CHECK(rename_intent_in_bounds(&t, clusters, dir_bytes));
}

static void test_decide(void) {
  CHECK(rename_intent_decide(false, false) == RENAME_INTENT_CLEAR);
  CHECK(rename_intent_decide(true, false) == RENAME_INTENT_CLEAR);
  CHECK(rename_intent_decide(false, true) == RENAME_INTENT_CLEAR);
  CHECK(rename_intent_decide(true, true) == RENAME_INTENT_COMPLETE);
}

int main(void) {
  test_pinned_vector();
  test_round_trip();
  test_one_flipped_byte_is_never_valid();
  test_decode_is_total();
  test_status_edges();
  test_in_bounds_edges();
  test_decide();
  return check_finish("rename_intent");
}
