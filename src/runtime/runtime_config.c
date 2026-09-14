/*
 * Patched configuration ownership and pre-libc validation for beam_server.
 * Validation is explicit because production builds define NDEBUG and cannot
 * rely on the assertion-only checks used by upstream examples.
 */
#include "runtime_config.h"
#include "runtime_network.h"

#include <sddf/network/lib_sddf_lwip.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

__attribute__((__section__(
    SERIAL_CLIENT_CONFIG_SECTION))) serial_client_config_t serial_config;
__attribute__((__section__(
    TIMER_CLIENT_CONFIG_SECTION))) timer_client_config_t timer_config;
__attribute__((
    __section__(FS_CLIENT_CONFIG_SECTION))) fs_client_config_t fs_config;

serial_queue_handle_t serial_tx_queue_handle;
serial_queue_handle_t serial_rx_queue_handle;

uintptr_t beam_heap_start;

static bool bytes_are_zero(const void *object, size_t size) {
  const unsigned char *bytes = object;
  for (size_t i = 0; i < size; i++) {
    if (bytes[i] != 0) {
      return false;
    }
  }
  return true;
}

static bool lwip_config_check_magic(const lib_sddf_lwip_config_t *config) {
  static const unsigned char magic[SDDF_LIB_SDDF_LWIP_MAGIC_LEN] = {
      's', 'D', 'D', 'F', 0x8};

  for (size_t i = 0; i < sizeof(magic); i++) {
    if ((unsigned char)config->magic[i] != magic[i]) {
      return false;
    }
  }
  return true;
}

runtime_status_t runtime_config_validate(bool *network_enabled) {
  if (!serial_config_check_magic(&serial_config)) {
    return RUNTIME_STATUS_CONFIG_SERIAL;
  }
  if (!timer_config_check_magic(&timer_config)) {
    return RUNTIME_STATUS_CONFIG_TIMER;
  }
  if (!fs_config_check_magic(&fs_config)) {
    return RUNTIME_STATUS_CONFIG_FS;
  }

  const bool net_zero = bytes_are_zero(&net_config, sizeof(net_config));
  const bool lwip_zero =
      bytes_are_zero(&lib_sddf_lwip_config, sizeof(lib_sddf_lwip_config));
  if (net_zero != lwip_zero) {
    return RUNTIME_STATUS_CONFIG_NET_PAIR;
  }
  if (net_zero) {
    *network_enabled = false;
    return RUNTIME_STATUS_OK;
  }
  if (!net_config_check_magic(&net_config)) {
    return RUNTIME_STATUS_CONFIG_NET;
  }
  if (!lwip_config_check_magic(&lib_sddf_lwip_config)) {
    return RUNTIME_STATUS_CONFIG_LWIP;
  }

  *network_enabled = true;
  return RUNTIME_STATUS_OK;
}

void runtime_serial_init(void) {
  if (serial_config.rx.queue.vaddr != NULL) {
    serial_queue_init(&serial_rx_queue_handle, serial_config.rx.queue.vaddr,
                      serial_config.rx.data.size, serial_config.rx.data.vaddr);
  }
  serial_queue_init(&serial_tx_queue_handle, serial_config.tx.queue.vaddr,
                    serial_config.tx.data.size, serial_config.tx.data.vaddr);
}
