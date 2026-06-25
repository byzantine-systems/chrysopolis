/*
 * Compile-time configuration for libmicrokitco in the beam_server PD.
 *
 * ERTS's helper threads (1 aux + poll + dirty-CPU/IO schedulers) run as
 * cothreads. 16 leaves headroom over the handful ERTS spawns with the
 * bounded scheduler flags.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#define LIBMICROKITCO_MAX_COTHREADS 32
