/*
 * libmicrokitco configuration for the beam_server runtime (process.c).
 *
 * MUST match nix/libmicrokitco_opts.h, which configures the libmicrokitco.a
 * built into the LionsOS stack: co_control_t's size depends on
 * LIBMICROKITCO_MAX_COTHREADS, and process.c statically allocates one.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#define LIBMICROKITCO_MAX_COTHREADS 32
