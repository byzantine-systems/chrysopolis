/*
 * Fields are assembled byte by byte, so host byte order never matters. Enum
 * switches have no default, so -Wswitch flags a missed case.
 */
#include "runtime_boot_select.h"

static uint64_t le64(const uint8_t *p) {
  uint64_t v = 0;
  for (size_t i = 0; i < 8; i++) {
    v |= (uint64_t)p[i] << (8 * i);
  }
  return v;
}

static uint32_t le32(const uint8_t *p) {
  uint32_t v = 0;
  for (size_t i = 0; i < 4; i++) {
    v |= (uint32_t)p[i] << (8 * i);
  }
  return v;
}

boot_config boot_config_decode(const uint8_t bytes[static boot_config_size]) {
  boot_config config = {.cls = boot_config_unpatched,
                        .profile = boot_profile_start,
                        .safe_below = 0};
  if (le64(bytes + boot_config_magic_off) != boot_config_magic) {
    return config;
  }
  if (le32(bytes + boot_config_version_off) != boot_config_version) {
    config.cls = boot_config_bad_version;
    return config;
  }
  for (size_t i = boot_config_reserved_off; i < boot_config_size; i++) {
    if (bytes[i] != 0) {
      config.cls = boot_config_bad_reserved;
      return config;
    }
  }
  const uint32_t profile = le32(bytes + boot_config_profile_off);
  if (profile >= boot_profile_count) {
    config.cls = boot_config_bad_profile;
    return config;
  }
  config.cls = boot_config_valid;
  config.profile = (boot_profile)profile; /* checked against the count above */
  config.safe_below = le32(bytes + boot_config_safe_below_off);
  return config;
}

bool boot_config_needs_observation(boot_config config) {
  return config.cls == boot_config_valid && config.profile == boot_profile_orch;
}

static boot_choice start_because(boot_reason reason, uint32_t remaining) {
  return (boot_choice){
      .profile = boot_profile_start, .reason = reason, .remaining = remaining};
}

boot_choice boot_select(boot_config config, boot_observation observation) {
  switch (config.cls) {
  case boot_config_unpatched:
    return start_because(boot_reason_unpatched, 0);
  case boot_config_bad_version:
    return start_because(boot_reason_bad_version, 0);
  case boot_config_bad_reserved:
    return start_because(boot_reason_bad_reserved, 0);
  case boot_config_bad_profile:
    return start_because(boot_reason_bad_profile, 0);
  case boot_config_valid:
    break;
  }
  switch (config.profile) {
  case boot_profile_start:
    return start_because(boot_reason_chosen, 0);
  case boot_profile_orch:
    break;
  }
  if (!observation.page_ok) {
    return start_because(boot_reason_no_page, 0);
  }
  /* Root never publishes a count above the budget, but a page is input: a
   * larger count reads as no budget left rather than wrapping. */
  const uint32_t remaining =
      observation.effective_budget > observation.lifetime_count
          ? observation.effective_budget - observation.lifetime_count
          : 0;
  if (remaining <= config.safe_below) {
    return start_because(boot_reason_budget_low, remaining);
  }
  if (!observation.boot_present) {
    return start_because(boot_reason_boot_missing, remaining);
  }
  return (boot_choice){.profile = boot_profile_orch,
                       .reason = boot_reason_chosen,
                       .remaining = remaining};
}

const char *boot_profile_name(boot_profile profile) {
  switch (profile) {
  case boot_profile_start:
    return "start";
  case boot_profile_orch:
    return "orch";
  }
  return "invalid";
}

const char *boot_reason_name(boot_reason reason) {
  switch (reason) {
  case boot_reason_unpatched:
    return "unpatched";
  case boot_reason_chosen:
    return "chosen";
  case boot_reason_bad_version:
    return "bad-version";
  case boot_reason_bad_reserved:
    return "bad-reserved";
  case boot_reason_bad_profile:
    return "bad-profile";
  case boot_reason_no_page:
    return "no-page";
  case boot_reason_budget_low:
    return "budget-low";
  case boot_reason_boot_missing:
    return "boot-missing";
  }
  return "invalid";
}
