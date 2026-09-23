/*
 * Final payload selection after the adapter services are ready.
 *
 * ERTS runs on a cothread rather than the Microkit root context. Once all
 * cothreads park, control falls back to init and enters the handler loop, whose
 * notifications wake the scheduler again. Calling erl_start on the root would
 * monopolise it and force every wait path to poll.
 */
#include "runtime_boot.h"
#include "runtime_cothread.h"
#include "runtime_network.h"
#include "runtime_restart.h"
#ifdef CHRYSO_DIAGNOSTIC
#include "runtime_thread_probe.h"
#endif

#include <libmicrokitco.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static void beam_erl_entry(void) {
  /* -MIscs caps the literal super carrier so it fits the configured heap.
   * -Bd disables the break-handler thread, which would block forever on this
   * console. erlexec normally translates +M flags and supplies the release
   * paths after --; this PD calls erl_start directly, so it passes both. */
  static char *argv[] = {
      "beam",
      "-MIscs",
      "256",
      "-Bd",
      "--",
      "-root",
      "/",
      "-bindir",
      "/bin",
      "-boot",
      "/releases/28/start",
      "-mode",
      "embedded",
      NULL,
  };
  erl_start(13, argv);
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
