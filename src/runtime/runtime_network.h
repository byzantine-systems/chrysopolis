#ifndef CHRYSOPOLIS_RUNTIME_NETWORK_H
#define CHRYSOPOLIS_RUNTIME_NETWORK_H 1

#include <lions/posix/posix.h>
#include <sddf/network/config.h>
#include <sddf/network/lib_sddf_lwip.h>
#include <sddf/network/queue.h>

#include "runtime_lifecycle.h"

/*
 * Static-lifetime network configuration populated by the Microkit image tool.
 * Callers borrow these objects. net_config must pass its magic check before
 * any embedded address is followed; the queue handles become usable only
 * after runtime_network_init initialises the linked lwIP stack.
 */
extern net_client_config_t net_config;
extern lib_sddf_lwip_config_t lib_sddf_lwip_config;
extern net_queue_handle_t net_rx_handle;
extern net_queue_handle_t net_tx_handle;

/*
 * Static socket operation table owned by tcp.c. Callers borrow it for the
 * lifetime of the PD and must not replace its callbacks.
 */
extern libc_socket_config_t socket_config;

/*
 * Process pending lwIP receive, timeout and transmit work. The network stack
 * must already be initialised. This is synchronous and returns after the
 * currently available work has been serviced.
 */
void beam_net_pump(void);

/* Initialise lwIP after libc, cothreads, and the filesystem are ready. */
void runtime_network_init(bool enabled);

/* Service network work belonging to one Microkit notification. */
void runtime_network_notified(microkit_channel ch);

/* Flush notification work deferred by the linked lwIP adapter. */
void runtime_network_flush(void);

/* Spawn the non-ERTS socket probe. */
[[nodiscard]] runtime_status_t runtime_network_start_probe(void);

#endif
