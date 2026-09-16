#ifndef CHRYSOPOLIS_RUNTIME_BOOT_H
#define CHRYSOPOLIS_RUNTIME_BOOT_H 1

#include <microkit.h>

#include "runtime_lifecycle.h"

/*
 * Optional ERTS entry points. A missing weak symbol compares equal to NULL.
 * Callers must test it before calling; neither function takes ownership of
 * its arguments. erl_start() is entered only after the runtime, libc and
 * cothread scheduler are ready.
 */
extern void erl_start(int argc, char **argv) __attribute__((weak));
extern void beam_process_external_events(microkit_channel ch)
    __attribute__((weak));

/* Spawn ERTS when linked, otherwise spawn the optional bring-up probe. */
[[nodiscard]] runtime_status_t runtime_payload_start(bool network_enabled);

/* Forward an event to ERTS when its optional hook is linked. */
void runtime_payload_external_event(microkit_channel ch);

#endif
