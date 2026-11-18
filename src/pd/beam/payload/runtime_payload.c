/*
 * Final payload selection after the adapter services are ready.
 *
 * ERTS runs on a cothread rather than the Microkit root context. Once all
 * cothreads park, control falls back to init and enters the handler loop, whose
 * notifications wake the scheduler again. Calling erl_start on the root would
 * monopolise it and force every wait path to poll.
 *
 * The boot script comes from runtime_boot_select: the patched
 * `.orchestrator_boot_config` asks for a profile, and Root's page and the
 * disk are consulted only for a valid `orch` request.
 */
#include "runtime_boot.h"
#include "runtime_boot_select.h"
#include "runtime_control_dev.h"
#include "runtime_cothread.h"
#include "runtime_network.h"
#include "runtime_restart.h"
#ifdef CHRYSO_DIAGNOSTIC
#include "runtime_thread_probe.h"
#endif

#include <libmicrokitco.h>
#include <runtime_release.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Zero until the image build patches it (runtime_boot_select.h has the
 * layout). Volatile so the compiler never folds the placeholder, static and
 * used because only objcopy names it. */
static volatile uint8_t orchestrator_boot_config[boot_config_size]
    __attribute__((__section__(".orchestrator_boot_config"), aligned(8),
                   used)) = {};

#define RELEASE_DIR "/releases/" CHRYSO_OTP_RELEASE

/* Per profile: the -boot argument, its file on disk, and an optional -config.
 */
static const struct {
  const char *boot;
  const char *boot_file;
  const char *config;
} boot_table[boot_profile_count] = {
    [boot_profile_start] = {RELEASE_DIR "/start", RELEASE_DIR "/start.boot",
                            nullptr},
    [boot_profile_orch] = {RELEASE_DIR "/orch", RELEASE_DIR "/orch.boot",
                           RELEASE_DIR "/orch"},
};

/* -MIscs caps the literal super carrier so it fits the configured heap.
 * -Bd disables the break-handler thread, which would block forever on this
 * console. erlexec normally translates +M flags and supplies the release
 * paths after --; this PD calls erl_start directly, so it passes both. */
static char *erts_argv[] = {
    "beam", "-MIscs", "256",      "-Bd",   "--",    "-root", "/",     "-bindir",
    "/bin", "-mode",  "embedded", "-boot", nullptr, nullptr, nullptr, nullptr,
};
/* Index of -boot's value, then the optional -config pair and the null. */
static constexpr size_t argv_boot = 12;
static_assert(sizeof erts_argv / sizeof erts_argv[0] >= argv_boot + 4);
static int erts_argc;

static boot_choice choose_boot(void) {
  uint8_t bytes[boot_config_size];
  for (size_t i = 0; i < boot_config_size; i++) {
    bytes[i] = orchestrator_boot_config[i];
  }
  const boot_config config = boot_config_decode(bytes);
  boot_observation observation = {};
  if (boot_config_needs_observation(config)) {
    observation.page_ok = runtime_control_dev_beam_row(
        &observation.lifetime_count, &observation.effective_budget);
    struct stat st = {};
    observation.boot_present =
        stat(boot_table[config.profile].boot_file, &st) == 0;
  }
  return boot_select(config, observation);
}

/* Fill erts_argv for profile. The casts only fit argv's char ** type: the
 * strings are static literals, as the hard-coded argv held before. */
static void set_boot(boot_profile profile) {
  erts_argv[argv_boot] = (char *)boot_table[profile].boot;
  size_t argc = argv_boot + 1;
  if (boot_table[profile].config != nullptr) {
    erts_argv[argc++] = "-config";
    erts_argv[argc++] = (char *)boot_table[profile].config;
  }
  erts_argv[argc] = nullptr;
  erts_argc = (int)argc;
}

static void beam_erl_entry(void) {
  erl_start(erts_argc, erts_argv);
  beam_request_restart(0);
}

static runtime_status_t set_erts_environment(void) {
  if (setenv("BINDIR", "/bin", 1) != 0 || setenv("ROOTDIR", "/", 1) != 0 ||
      setenv("EMU", "beam", 1) != 0 || setenv("PROGNAME", "beam", 1) != 0) {
    return RUNTIME_STATUS_ENVIRONMENT;
  }
  return RUNTIME_STATUS_OK;
}

runtime_status_t runtime_payload_start(bool network_enabled) {
#ifdef CHRYSO_DIAGNOSTIC
  if (!runtime_thread_probe_run()) {
    return RUNTIME_STATUS_THREAD_PROBE;
  }
#endif
  if (erl_start != NULL) {
    const runtime_status_t environment_status = set_erts_environment();
    if (environment_status != RUNTIME_STATUS_OK) {
      return environment_status;
    }

    const boot_choice choice = choose_boot();
    set_boot(choice.profile);
    printf("BEAM|boot|profile=%s|reason=%s|remaining=%u\n",
           boot_profile_name(choice.profile), boot_reason_name(choice.reason),
           (unsigned int)choice.remaining);
    printf("Handing off to ERTS core loop...\n");
    if (microkit_cothread_spawn(beam_erl_entry, NULL) ==
        LIBMICROKITCO_NULL_HANDLE) {
      return RUNTIME_STATUS_PAYLOAD_SPAWN;
    }
    thread_run_cothreads();
    return RUNTIME_STATUS_OK;
  }

  printf("liberts.a not linked: bring-up mode (console + clock + heap).\n");
  if (!network_enabled) {
    printf("SOCKET_SMOKE|SKIP: net config absent\n");
    return RUNTIME_STATUS_OK;
  }
  return runtime_network_start_probe();
}

void runtime_payload_external_event(microkit_channel ch) {
  if (beam_process_external_events != NULL) {
    beam_process_external_events(ch);
  }
}
