/*
 * LionsOS filesystem client setup and warm-start reconciliation.
 * Client bookkeeping is restored with beam_server, but the shared queues
 * survive and must be drained before request identifiers are reused. Waiting
 * for ordinary commands deliberately processes completions synchronously and
 * does not yield to other cothreads: overlapping filesystem commands would
 * alias fs_share buffers and can load a BEAM file with another command's data.
 */
#include "runtime_fs.h"
#include "runtime_config.h"

#include <libmicrokitco.h>
#include <lions/fs/helpers.h>
#include <lions/fs/protocol.h>

#include <stdint.h>
#include <stdio.h>

fs_queue_t *fs_command_queue;
fs_queue_t *fs_completion_queue;
char *fs_share;

static void fs_blocking_wait(microkit_channel ch) {
  (void)ch;
  fs_process_completions(NULL);
}

void runtime_fs_bind(void) {
  fs_command_queue = fs_config.server.command_queue.vaddr;
  fs_completion_queue = fs_config.server.completion_queue.vaddr;
  fs_share = fs_config.server.share.vaddr;
  fs_set_blocking_wait(fs_blocking_wait);
}

static void fs_discard_stale(void) {
  /* fatfs may still be executing a command from the dead instance. Wait for
   * the command queue to empty before discarding completions, otherwise a late
   * stale completion could reuse the new mount's request id. */
  while (__atomic_load_n(&fs_command_queue->head, __ATOMIC_ACQUIRE) !=
         fs_command_queue->tail) {
    microkit_cothread_yield();
  }

  const uint64_t stale = fs_queue_length_consumer(fs_completion_queue);
  fs_queue_publish_consumption(fs_completion_queue, stale);
  printf("BEAM|restart|fs-discarded=%llu\n", (unsigned long long)stale);
}

runtime_status_t runtime_fs_start(bool warm_start) {
  if (warm_start) {
    fs_discard_stale();
  }

  fs_cmpl_t completion = {};
  const int error =
      fs_command_blocking(&completion, (fs_cmd_t){.type = FS_CMD_INITIALISE});
  if (error != 0) {
    return RUNTIME_STATUS_FS_COMMAND;
  }
  if (completion.status != FS_STATUS_SUCCESS) {
    return RUNTIME_STATUS_FS_MOUNT;
  }

  printf("FAT filesystem mounted via fs_server.\n");
  return RUNTIME_STATUS_OK;
}
