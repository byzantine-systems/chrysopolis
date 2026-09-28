#ifndef CHRYSOPOLIS_HOST_ABI_SHIM_H
#define CHRYSOPOLIS_HOST_ABI_SHIM_H 1

#include <stddef.h>
#include <stdint.h>

/* Runs the generated checker `name` (length-delimited, not NUL-terminated)
 * over bytes[0, len); `bank` selects the spec bank and is ignored by the other
 * checkers. Returns the chryso_abi_reject verdict, or UINT8_MAX for an unknown
 * checker name. */
uint8_t abi_shim_check(const char *name, size_t name_len, const uint8_t *bytes,
                       size_t len, size_t bank);

#endif
