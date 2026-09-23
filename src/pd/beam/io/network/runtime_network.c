/*
 * Linked lwIP lifecycle and the non-ERTS socket smoke probe.
 *
 * The Microkit callback is the primary network driver. Timer and RX work must
 * run before blocked cothreads wake so socket readiness observes fresh state.
 * Poll shims also call beam_net_pump as a latency backstop while the root has
 * not yet returned to the handler loop.
 *
 * Queue memory belongs to the generated topology and survives beam_server
 * restart. A cold boot seeds the TX free ring; a warm boot only rebuilds local
 * handles and lets the virtualiser return descriptors already in flight.
 */
#include "runtime_network.h"
#include "runtime_config.h"
#include "runtime_restart.h"
#include "runtime_timer.h"

#include <libmicrokitco.h>
#include <microkit.h>
#include <sddf/timer/client.h>
#include <sddf/timer/protocol.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

__attribute__((
    __section__(NET_CLIENT_CONFIG_SECTION))) net_client_config_t net_config;
__attribute__((__section__(
    LWIP_CONFIG_SECTION))) lib_sddf_lwip_config_t lib_sddf_lwip_config;

net_queue_handle_t net_rx_handle;
net_queue_handle_t net_tx_handle;

static bool lwip_up;
static bool dhcp_ready;

#define NET_TIMEOUT (100 * NS_IN_MS)

static void netif_status_callback(char *ip_addr) {
  printf("SOCKET_SMOKE|DHCP: %s\n", ip_addr);
  dhcp_ready = true;
}

void runtime_network_init(bool enabled) {
  if (!enabled) {
    return;
  }

  net_queue_init(&net_rx_handle, net_config.rx.free_queue.vaddr,
                 net_config.rx.active_queue.vaddr, net_config.rx.num_buffers);
  net_queue_init(&net_tx_handle, net_config.tx.free_queue.vaddr,
                 net_config.tx.active_queue.vaddr, net_config.tx.num_buffers);

  if (beam_warm_start()) {
    const uint16_t have = net_queue_length(net_tx_handle.free);
    printf("BEAM|restart|net-tx-free=%u/%u\n", (unsigned)have,
           (unsigned)net_config.tx.num_buffers);
  } else {
    net_buffers_init(&net_tx_handle, 0);
  }

  sddf_lwip_init(&lib_sddf_lwip_config, &net_config, &timer_config,
                 net_rx_handle, net_tx_handle, NULL, printf,
                 netif_status_callback, NULL, NULL, NULL);
  lwip_up = true;
  sddf_lwip_maybe_notify();
  beam_timer_arm(sddf_timer_time_now(timer_config.driver_id) + NET_TIMEOUT);
}

void runtime_network_flush(void) {
  if (lwip_up) {
    sddf_lwip_maybe_notify();
  }
}

static void flush_deferred_notify(void) {
  /* sddf_lwip_maybe_notify uses Microkit's one-slot deferred signal. A pump
   * running on a cothread cannot wait for a callback epilogue to flush it, so
   * deliver it here before the root can block in seL4_Recv. */
  if (microkit_have_signal) {
    microkit_have_signal = seL4_False;
    seL4_Send(microkit_signal_cap, microkit_signal_msg);
  }
}

void beam_net_pump(void) {
  static unsigned pump_count;
  if (!lwip_up) {
    return;
  }
  sddf_lwip_process_rx();
  if ((++pump_count & 63) == 0) {
    sddf_lwip_process_timeout();
  }
  sddf_lwip_maybe_notify();
  flush_deferred_notify();
}

void runtime_network_notified(microkit_channel ch) {
  if (!lwip_up) {
    return;
  }
  if (ch == timer_config.driver_id) {
    sddf_lwip_process_rx();
    sddf_lwip_process_timeout();
    beam_timer_arm(sddf_timer_time_now(timer_config.driver_id) + NET_TIMEOUT);
  } else if (ch == net_config.rx.id) {
    sddf_lwip_process_rx();
  }
}

static void socket_smoke(void) {
  int server = -1;
  int client = -1;
  const char *failure = NULL;
  int failure_errno = 0;

  printf("SOCKET_SMOKE|START\n");
  while (!dhcp_ready) {
    microkit_cothread_yield();
  }

  server = socket(AF_INET, SOCK_STREAM, 0);
  if (server < 0) {
    failure = "socket";
    failure_errno = errno;
    goto cleanup;
  }

  struct sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(7777);
  address.sin_addr.s_addr = INADDR_ANY;
  if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0) {
    failure = "bind";
    failure_errno = errno;
    goto cleanup;
  }
  if (listen(server, 1) != 0) {
    failure = "listen";
    failure_errno = errno;
    goto cleanup;
  }
  if (close(server) != 0) {
    failure = "close-server";
    failure_errno = errno;
    server = -1;
    goto cleanup;
  }
  server = -1;

  client = socket(AF_INET, SOCK_STREAM, 0);
  if (client < 0) {
    failure = "socket2";
    failure_errno = errno;
    goto cleanup;
  }
  if (fcntl(client, F_SETFL, O_NONBLOCK) != 0) {
    failure = "fcntl";
    failure_errno = errno;
    goto cleanup;
  }

  struct sockaddr_in host = {};
  host.sin_family = AF_INET;
  host.sin_port = htons(9);
  host.sin_addr.s_addr = inet_addr("10.0.2.2");
  const int connect_result =
      connect(client, (struct sockaddr *)&host, sizeof(host));
  if (connect_result != 0 && errno != EINPROGRESS) {
    failure = "connect";
    failure_errno = errno;
    goto cleanup;
  }

cleanup:
  if (client >= 0) {
    (void)close(client);
  }
  if (server >= 0) {
    (void)close(server);
  }
  if (failure != NULL) {
    printf("SOCKET_SMOKE|FAIL: %s errno=%d\n", failure, failure_errno);
    return;
  }
  printf("SOCKET_SMOKE|PASS\n");
}

runtime_status_t runtime_network_start_probe(void) {
  if (microkit_cothread_spawn(socket_smoke, NULL) ==
      LIBMICROKITCO_NULL_HANDLE) {
    return RUNTIME_STATUS_PROBE_SPAWN;
  }
  return RUNTIME_STATUS_OK;
}
