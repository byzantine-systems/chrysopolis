/* runtime_boot_select.h: the boot profile's laws, then anchors.
 *
 * Planted defects (each run on a copy of the tree):
 * | Defect | Caught by |
 * |---|---|
 * | unpatched selects orch | identity |
 * | the budget check uses < for <= | anchors |
 * | a bad version decodes as valid | totality, anchors |
 * | no page boots orch | no_page |
 * | the magic constant mistyped | magic_spells_orchboot |
 */
#include "check.h"
#include "prop.h"

#include "runtime_boot_select.h"

#include <stdint.h>
#include <string.h>

static constexpr size_t rounds = 20000;

static void put_le32(uint8_t *p, uint32_t v) {
  for (size_t i = 0; i < 4; i++) {
    p[i] = (uint8_t)(v >> (8 * i));
  }
}

/* A version 1 section, as the image build writes it: the magic as text. */
static void encode(uint8_t out[static boot_config_size], uint32_t version,
                   uint32_t profile, uint32_t safe_below) {
  memset(out, 0, boot_config_size);
  memcpy(out + boot_config_magic_off, "ORCHBOOT", 8);
  put_le32(out + boot_config_version_off, version);
  put_le32(out + boot_config_profile_off, profile);
  put_le32(out + boot_config_safe_below_off, safe_below);
}

static uint32_t draw_u32(prop_rng *rng) { return (uint32_t)prop_next(rng); }

/* Small values often, so draws land on and around thresholds. */
static uint32_t draw_count(prop_rng *rng) {
  return prop_chance(rng, 80) ? (uint32_t)prop_below(rng, 70) : draw_u32(rng);
}

static boot_observation draw_observation(prop_rng *rng) {
  return (boot_observation){.page_ok = prop_chance(rng, 75),
                            .lifetime_count = draw_count(rng),
                            .effective_budget = draw_count(rng),
                            .boot_present = prop_chance(rng, 85)};
}

static void draw_bytes(prop_rng *rng, uint8_t out[static boot_config_size]) {
  for (size_t i = 0; i < boot_config_size; i++) {
    out[i] = (uint8_t)prop_next(rng);
  }
}

/* The magic constant and the text the image build writes are one value. */
static void test_magic_spells_orchboot(void) {
  uint8_t bytes[8] = {};
  for (size_t i = 0; i < 8; i++) {
    bytes[i] = (uint8_t)(boot_config_magic >> (8 * i));
  }
  CHECK(memcmp(bytes, "ORCHBOOT", 8) == 0);
}

/* Identity: without the magic, every input boots start, unpatched. */
static void test_identity(void) {
  prop_rng rng = prop_seed(0xb0071);
  uint8_t zero[boot_config_size] = {};
  CHECK(boot_config_decode(zero).cls == boot_config_unpatched);
  for (size_t n = 0; n < rounds; n++) {
    uint8_t bytes[boot_config_size];
    draw_bytes(&rng, bytes);
    if (memcmp(bytes, "ORCHBOOT", 8) == 0) {
      continue;
    }
    const boot_config config = boot_config_decode(bytes);
    const boot_choice choice = boot_select(config, draw_observation(&rng));
    CHECK(config.cls == boot_config_unpatched);
    CHECK(choice.profile == boot_profile_start);
    CHECK(choice.reason == boot_reason_unpatched);
    CHECK(!boot_config_needs_observation(config));
  }
}

/* Totality: any bytes behind a valid magic decode to a named class, and the
 * choice always has a profile and a named reason. */
static void test_totality(void) {
  prop_rng rng = prop_seed(0xb0072);
  for (size_t n = 0; n < rounds; n++) {
    uint8_t bytes[boot_config_size];
    draw_bytes(&rng, bytes);
    memcpy(bytes, "ORCHBOOT", 8);
    if (prop_chance(&rng, 50)) {
      put_le32(bytes + boot_config_version_off, boot_config_version);
    }
    if (prop_chance(&rng, 50)) {
      memset(bytes + boot_config_reserved_off, 0,
             boot_config_size - boot_config_reserved_off);
    }
    const boot_config config = boot_config_decode(bytes);
    const boot_choice choice = boot_select(config, draw_observation(&rng));
    CHECK(config.cls != boot_config_unpatched);
    CHECK(choice.profile < boot_profile_count);
    CHECK(strcmp(boot_reason_name(choice.reason), "invalid") != 0);
    CHECK(strcmp(boot_profile_name(choice.profile), "invalid") != 0);
    if (config.cls != boot_config_valid) {
      CHECK(choice.profile == boot_profile_start);
    }
  }
}

/* Only down: the result is start or the configured profile. */
static void test_only_down(void) {
  prop_rng rng = prop_seed(0xb0073);
  for (size_t n = 0; n < rounds; n++) {
    uint8_t bytes[boot_config_size];
    const uint32_t profile = (uint32_t)prop_below(&rng, boot_profile_count);
    encode(bytes, boot_config_version, profile, draw_count(&rng));
    const boot_config config = boot_config_decode(bytes);
    const boot_choice choice = boot_select(config, draw_observation(&rng));
    CHECK(config.cls == boot_config_valid);
    CHECK(choice.profile == boot_profile_start ||
          choice.profile == config.profile);
    CHECK(boot_config_needs_observation(config) ==
          (config.profile == boot_profile_orch));
  }
}

/* Monotone: start for lack of budget at remaining r implies start for lack
 * of budget at every smaller remaining. */
static void test_monotone(void) {
  prop_rng rng = prop_seed(0xb0074);
  for (size_t n = 0; n < rounds; n++) {
    uint8_t bytes[boot_config_size];
    encode(bytes, boot_config_version, boot_profile_orch, draw_count(&rng));
    const boot_config config = boot_config_decode(bytes);
    boot_observation obs = draw_observation(&rng);
    obs.page_ok = true;
    obs.effective_budget = (uint32_t)prop_below(&rng, 80);
    obs.lifetime_count = (uint32_t)prop_below(&rng, obs.effective_budget + 1);
    const boot_choice at = boot_select(config, obs);
    if (at.reason != boot_reason_budget_low || obs.effective_budget == 0) {
      continue;
    }
    for (uint32_t count = obs.lifetime_count; count <= obs.effective_budget;
         count++) {
      obs.lifetime_count = count;
      CHECK(boot_select(config, obs).reason == boot_reason_budget_low);
    }
  }
}

/* No page, no orchestrator. */
static void test_no_page(void) {
  prop_rng rng = prop_seed(0xb0075);
  for (size_t n = 0; n < rounds; n++) {
    uint8_t bytes[boot_config_size];
    encode(bytes, boot_config_version, boot_profile_orch, draw_count(&rng));
    boot_observation obs = draw_observation(&rng);
    obs.page_ok = false;
    const boot_choice choice = boot_select(boot_config_decode(bytes), obs);
    CHECK(choice.profile == boot_profile_start);
    CHECK(choice.reason == boot_reason_no_page);
  }
}

static boot_choice orch_with(uint32_t safe_below, boot_observation obs) {
  uint8_t bytes[boot_config_size];
  encode(bytes, boot_config_version, boot_profile_orch, safe_below);
  return boot_select(boot_config_decode(bytes), obs);
}

/* Anchors for what the laws leave open: the boundary and the precedence. */
static void test_anchors(void) {
  const boot_observation at = {.page_ok = true,
                               .lifetime_count = 2,
                               .effective_budget = 64,
                               .boot_present = true};
  /* 62 left with safe_below 62 is safe mode, 63 left is not. */
  CHECK_EQ_U64(orch_with(62, at).reason, boot_reason_budget_low);
  CHECK_EQ_U64(orch_with(62, at).remaining, 62);
  boot_observation one_more = at;
  one_more.lifetime_count = 1;
  CHECK_EQ_U64(orch_with(62, one_more).reason, boot_reason_chosen);
  CHECK_EQ_U64(orch_with(62, one_more).profile, boot_profile_orch);
  /* No page beats a low budget, and a low budget beats a missing file. */
  boot_observation blind = at;
  blind.page_ok = false;
  CHECK_EQ_U64(orch_with(62, blind).reason, boot_reason_no_page);
  boot_observation missing = at;
  missing.boot_present = false;
  CHECK_EQ_U64(orch_with(62, missing).reason, boot_reason_budget_low);
  CHECK_EQ_U64(orch_with(0, missing).reason, boot_reason_boot_missing);
  /* A version the C does not know boots start. */
  uint8_t bytes[boot_config_size];
  encode(bytes, 2, boot_profile_orch, 0);
  const boot_choice v2 = boot_select(boot_config_decode(bytes), at);
  CHECK_EQ_U64(v2.profile, boot_profile_start);
  CHECK_EQ_U64(v2.reason, boot_reason_bad_version);
}

int main(void) {
  test_magic_spells_orchboot();
  test_identity();
  test_totality();
  test_only_down();
  test_monotone();
  test_no_page();
  test_anchors();
  return check_finish("boot_select");
}
