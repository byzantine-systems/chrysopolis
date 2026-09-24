/* runtime_status.h and runtime_futex_cmd.h: failure policy and op decoding. */
#include "check.h"

#include "runtime_futex_cmd.h"
#include "runtime_status.h"

#include <limits.h>
#include <string.h>

static const runtime_status_t all_statuses[] = {
    RUNTIME_STATUS_OK,
    RUNTIME_STATUS_CONFIG_SERIAL,
    RUNTIME_STATUS_CONFIG_TIMER,
    RUNTIME_STATUS_CONFIG_FS,
    RUNTIME_STATUS_CONFIG_NET,
    RUNTIME_STATUS_CONFIG_LWIP,
    RUNTIME_STATUS_CONFIG_NET_PAIR,
    RUNTIME_STATUS_COTHREAD_ALLOC,
    RUNTIME_STATUS_FS_COMMAND,
    RUNTIME_STATUS_FS_MOUNT,
    RUNTIME_STATUS_CLOCK,
    RUNTIME_STATUS_ENVIRONMENT,
    RUNTIME_STATUS_PAYLOAD_SPAWN,
    RUNTIME_STATUS_PROBE_SPAWN,
    RUNTIME_STATUS_THREAD_PROBE,
};
static constexpr size_t status_count =
    sizeof(all_statuses) / sizeof(all_statuses[0]);

/* Only configuration failures park; everything else asks Root to restart. */
static void test_config_failure_policy(void) {
  for (size_t i = 0; i < status_count; i++) {
    const runtime_status_t status = all_statuses[i];
    const bool config = strcmp(runtime_status_stage(status), "config") == 0;
    CHECK(runtime_status_is_config_failure(status) == config);
  }
  CHECK(!runtime_status_is_config_failure(RUNTIME_STATUS_OK));
  CHECK(!runtime_status_is_config_failure((runtime_status_t)7));
}

static void test_every_status_has_text(void) {
  for (size_t i = 0; i < status_count; i++) {
    const runtime_status_t status = all_statuses[i];
    CHECK(strcmp(runtime_status_name(status), "unknown") != 0);
    CHECK(strcmp(runtime_status_stage(status), "unknown") != 0);
    /* Names are distinct, so the console line identifies the failure. */
    for (size_t j = 0; j < i; j++) {
      CHECK(strcmp(runtime_status_name(status),
                   runtime_status_name(all_statuses[j])) != 0);
    }
    CHECK(runtime_status_exit_code(status) == (uint8_t)status);
  }
  CHECK(strcmp(runtime_status_name((runtime_status_t)200), "unknown") == 0);
  CHECK(strcmp(runtime_status_stage((runtime_status_t)200), "unknown") == 0);
}

static void test_format_code(void) {
  char text[runtime_status_code_text_size] = {'x', 'x', 'x', 'x'};
  CHECK_EQ_U64(runtime_status_format_code(RUNTIME_STATUS_OK, text), 1);
  CHECK(strcmp(text, "0") == 0);
  CHECK_EQ_U64(runtime_status_format_code(RUNTIME_STATUS_THREAD_PROBE, text),
               2);
  CHECK(strcmp(text, "39") == 0);

  /* Out-of-enum values report their low byte, the code Root receives, and
   * never need more than three digits. */
  CHECK_EQ_U64(runtime_status_format_code((runtime_status_t)255, text), 3);
  CHECK(strcmp(text, "255") == 0);
  CHECK_EQ_U64(runtime_status_format_code((runtime_status_t)256, text), 1);
  CHECK(strcmp(text, "0") == 0);
  CHECK_EQ_U64(runtime_status_format_code((runtime_status_t)1001, text), 3);
  CHECK(strcmp(text, "233") == 0);
  CHECK(runtime_status_exit_code((runtime_status_t)1001) == 233);
}

static void test_futex_classify(void) {
  static constexpr int private_flag = 128;
  static constexpr int clock_realtime = 256;

  CHECK(runtime_futex_classify(0) == runtime_futex_wait);
  CHECK(runtime_futex_classify(9) == runtime_futex_wait);
  CHECK(runtime_futex_classify(1) == runtime_futex_wake);
  CHECK(runtime_futex_classify(10) == runtime_futex_wake);
  CHECK(runtime_futex_classify(0 | private_flag) == runtime_futex_wait);
  CHECK(runtime_futex_classify(9 | private_flag | clock_realtime) ==
        runtime_futex_wait);
  CHECK(runtime_futex_classify(1 | private_flag) == runtime_futex_wake);

  /* FD, REQUEUE, CMP_REQUEUE, WAKE_OP, the PI family and unused numbers. */
  for (int cmd = 2; cmd <= 0x7f; cmd++) {
    if (cmd == 9 || cmd == 10) {
      continue;
    }
    CHECK(runtime_futex_classify(cmd) == runtime_futex_unsupported);
    CHECK(runtime_futex_classify(cmd | private_flag) ==
          runtime_futex_unsupported);
  }
  CHECK(runtime_futex_classify(-1) == runtime_futex_unsupported);
  CHECK(runtime_futex_classify(INT_MIN) == runtime_futex_wait);
}

int main(void) {
  test_config_failure_policy();
  test_every_status_has_text();
  test_format_code();
  test_futex_classify();
  return check_finish("status");
}
