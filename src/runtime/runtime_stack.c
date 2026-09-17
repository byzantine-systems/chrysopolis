/*
 * Stack acquisition for libmicrokitco. The output array doubles as the record
 * of what this call owns, so rollback needs no second array sized by the
 * cothread limit.
 */
#include "runtime_stack.h"

bool runtime_stack_prepare_n(uintptr_t stacks[], size_t count, size_t bytes,
                             runtime_stack_alloc_fn allocate,
                             runtime_stack_free_fn release, void *context) {
  if (stacks == nullptr || count == 0 || bytes < runtime_stack_min_bytes ||
      allocate == nullptr || release == nullptr) {
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    stacks[i] = 0;
  }
  for (size_t i = 0; i < count; i++) {
    void *stack = allocate(bytes, context);
    if (stack == nullptr || (uintptr_t)stack > UINTPTR_MAX - bytes) {
      if (stack != nullptr) {
        release(stack, context);
      }
      for (size_t j = i; j > 0; j--) {
        release((void *)stacks[j - 1], context);
        stacks[j - 1] = 0;
      }
      return false;
    }
    stacks[i] = (uintptr_t)stack;
  }
  return true;
}
