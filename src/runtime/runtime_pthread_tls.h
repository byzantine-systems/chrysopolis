#ifndef CHRYSOPOLIS_RUNTIME_PTHREAD_TLS_H
#define CHRYSOPOLIS_RUNTIME_PTHREAD_TLS_H 1

#include <libmicrokitco.h>

/* TLS belongs to a cothread slot while it runs. Call clear before reusing a
 * slot, and finish before destroying a thread. An out-of-range handle is
 * ignored; both functions run on the single TCB and may invoke destructors
 * only from finish. Destructors may set a key again, so finish makes the
 * POSIX-defined number of passes before discarding remaining values. */
enum { RUNTIME_TSD_DESTRUCTOR_PASSES = 4 };
void runtime_tls_clear(microkit_cothread_ref_t handle);
void runtime_tls_finish(microkit_cothread_ref_t handle);

#endif
