/*
 * SMP core spawner: binds secondary scheduler thread contexts (TCBs)
 * to the primary execution space.
 *
 * If Microkit's 1:1 TCB/VSpace rule blocks multi-threaded protection domains,
 * schedulers move to separate PDs joined by shared memory regions instead.
 */
#include <microkit.h>
#include <sel4/sel4.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_SCHEDULERS 4
#define SECONDARY_THREAD_STACK_SIZE 65536
#define BASE_OUTPUT_CAP 0x100 /* system-defined capability offset base */

static uint8_t scheduler_stacks[MAX_SCHEDULERS][SECONDARY_THREAD_STACK_SIZE]
    __attribute__((aligned(16)));

void spawn_secondary_smp_scheduler(int worker_id, void (*entry_point)(void)) {
  seL4_CPtr tcb_cap = BASE_OUTPUT_CAP + worker_id;
  uintptr_t stack_top =
      (uintptr_t)&scheduler_stacks[worker_id][SECONDARY_THREAD_STACK_SIZE];

  seL4_UserContext context = {0};
  context.pc = (seL4_Word)entry_point;
  context.sp = (seL4_Word)stack_top;

  long error = seL4_TCB_WriteRegisters(
      tcb_cap, false, 0, sizeof(seL4_UserContext) / sizeof(seL4_Word),
      &context);
  if (error == seL4_NoError) {
    seL4_TCB_Resume(tcb_cap);
  }
}
