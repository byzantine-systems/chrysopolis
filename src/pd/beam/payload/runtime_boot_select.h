#ifndef CHRYSOPOLIS_RUNTIME_BOOT_SELECT_H
#define CHRYSOPOLIS_RUNTIME_BOOT_SELECT_H 1

/*
 * Which boot script ERTS starts.
 *
 * `.orchestrator_boot_config` is 32 little-endian bytes the image build may
 * patch: magic "ORCHBOOT" (0), version (8), profile (12), safe_below (16),
 * zero reserved bytes (20). Unpatched means `start`, so unpatched images are
 * unchanged. `orch` boots the orchestrator unless Root's page shows at most
 * `safe_below` BEAM restarts left, then `start` (safe mode). Root owns the
 * count, this unit only reads it. Pure, and total over every input.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static constexpr size_t boot_config_size = 32;
static constexpr size_t boot_config_magic_off = 0;
static constexpr size_t boot_config_version_off = 8;
static constexpr size_t boot_config_profile_off = 12;
static constexpr size_t boot_config_safe_below_off = 16;
static constexpr size_t boot_config_reserved_off = 20;
/* The ASCII bytes "ORCHBOOT" read little-endian. The image build writes the
 * text, not this number, and the host suite checks that the two agree. */
static constexpr uint64_t boot_config_magic = UINT64_C(0x544f4f424843524f);
static constexpr uint32_t boot_config_version = 1;

typedef enum {
  boot_profile_start,
  boot_profile_orch,
} boot_profile;

static constexpr size_t boot_profile_count = 2;

/* What the section says, before any observation. */
typedef enum {
  boot_config_unpatched,
  boot_config_valid,
  boot_config_bad_version,
  boot_config_bad_reserved,
  boot_config_bad_profile,
} boot_config_class;

typedef struct {
  boot_config_class cls;
  /* Meaningful only when cls is boot_config_valid. */
  boot_profile profile;
  uint32_t safe_below;
} boot_config;

/* What the adapter saw. Read only for a valid orch config. */
typedef struct {
  bool page_ok;
  uint32_t lifetime_count;
  uint32_t effective_budget;
  bool boot_present;
} boot_observation;

typedef enum {
  boot_reason_unpatched,
  boot_reason_chosen,
  boot_reason_bad_version,
  boot_reason_bad_reserved,
  boot_reason_bad_profile,
  boot_reason_no_page,
  boot_reason_budget_low,
  boot_reason_boot_missing,
} boot_reason;

typedef struct {
  boot_profile profile;
  boot_reason reason;
  /* Restarts Root still allows the BEAM, 0 when no page was read. */
  uint32_t remaining;
} boot_choice;

/* Decode the section. Every byte string is an expected input: one that is not
 * exactly a version 1 config is classified, and only boot_config_valid
 * carries a profile. Unchecked: bytes must point to boot_config_size readable
 * bytes. */
[[__nodiscard__]] boot_config
boot_config_decode(const uint8_t bytes[static boot_config_size]);

/* Whether the adapter must read the page and the boot file at all: only for
 * a valid orch config, so every other image does neither. */
[[__nodiscard__]] bool boot_config_needs_observation(boot_config config);

/* The profile to boot and why. Laws (suite_boot_select.c):
 * - identity: anything but a valid config boots start.
 * - only down: the result is start or the configured profile.
 * - monotone: start for lack of budget at remaining r implies start at
 *   every remaining <= r.
 * - no page, no orchestrator: a valid orch config without a page boots start.
 */
[[__nodiscard__]] boot_choice boot_select(boot_config config,
                                          boot_observation observation);

/* Stable lower-case names for the console line, static storage. An
 * out-of-range value (a cast integer) yields "invalid". */
[[__nodiscard__]] const char *boot_profile_name(boot_profile profile);
[[__nodiscard__]] const char *boot_reason_name(boot_reason reason);

#endif
