#ifndef CHRYSOPOLIS_RUNTIME_CONFIG_H
#define CHRYSOPOLIS_RUNTIME_CONFIG_H 1

#include <lions/fs/config.h>
#include <lions/fs/protocol.h>
#include <sddf/serial/config.h>
#include <sddf/serial/queue.h>
#include <sddf/timer/config.h>

#include <stdint.h>

/*
 * Static-lifetime configuration blobs populated by the Microkit image tool
 * before _start. Callers borrow these objects and must validate their magic
 * before following any embedded address. They are readable before libc is
 * initialised and must never be freed or replaced.
 */
extern serial_client_config_t serial_config;
extern timer_client_config_t timer_config;
extern fs_client_config_t fs_config;

/*
 * Static-lifetime queue handles owned and initialised by main.c. Consumers
 * borrow them after the corresponding configuration has been validated and
 * initialisation has completed. The fs pointers refer to shared memory owned
 * by the generated system topology.
 */
extern serial_queue_handle_t serial_rx_queue_handle;
extern serial_queue_handle_t serial_tx_queue_handle;
extern fs_queue_t *fs_command_queue;
extern fs_queue_t *fs_completion_queue;
extern char *fs_share;

/*
 * Test-only restart channel configuration patched into the ELF by objcopy.
 * The dimensions and sentinel are part of the current C/image contract. The
 * production image leaves every entry at PD_RESTART_CH_NONE.
 */
#define PD_RESTART_CH_NONE 0xff
#define PD_RESTART_CLASS_COUNT 4
#define PD_RESTART_MODE_COUNT 2
#define PD_RESTART_MODE_HEALTHY 0
#define PD_RESTART_MODE_FAULT 1
extern volatile uint8_t pd_restart_channels[PD_RESTART_MODE_COUNT]
                                           [PD_RESTART_CLASS_COUNT];

/*
 * Base of the Microkit-provided libc arena. The image tool patches this value
 * before _start; libc_init() receives the region but does not own this symbol.
 */
extern uintptr_t beam_heap_start;

#endif
