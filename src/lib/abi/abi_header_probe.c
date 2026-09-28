/* Diagnostic-only: compile one generated orchestration header alone and twice.
 * With CHRYSO_ABI_WITH_RUNTIME_ABI it also shares a unit with runtime_abi.h,
 * which rejects a name collision between the two generated contracts. */
#ifndef CHRYSO_ABI_HEADER
#error "CHRYSO_ABI_HEADER must name one generated <chrysopolis/...> header"
#endif

#include CHRYSO_ABI_HEADER
#include CHRYSO_ABI_HEADER

#ifdef CHRYSO_ABI_WITH_RUNTIME_ABI
#include "runtime_abi.h"
#endif
