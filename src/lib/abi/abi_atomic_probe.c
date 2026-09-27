/* Diagnostic-only linked AArch64 probe. Every hot _Atomic field must lower to
 * inline loads, stores and exclusives: the exe links with no compiler_rt or
 * libatomic, so an __atomic_* or outline __aarch64_* helper fails the link. */
#include <chrysopolis/root_control.h>
#include <chrysopolis/worker_identity.h>
#include <chrysopolis/worker_protocol.h>
#include <chrysopolis/worker_status.h>

/* Layout is asserted by the headers; this only checks how the atomics lower. */
static chryso_root_status_page status_page;
static chryso_spec_page spec_page;
static chryso_worker_identity identity_page;
static chryso_worker_status worker_status_page;
static chryso_journal_page journal_page;

/* Keeps every load and exchange result observable. */
static volatile uint64_t sink;

/* Release/acquire are the orders the seqlock and journal protocols use. */
static void touch_u32(_Atomic(uint32_t) *field) {
  atomic_store_explicit(field, 2u, memory_order_release);
  sink = atomic_load_explicit(field, memory_order_acquire);
  sink = atomic_exchange_explicit(field, 4u, memory_order_acq_rel);
}

static void touch_u64(_Atomic(uint64_t) *field) {
  atomic_store_explicit(field, 2u, memory_order_release);
  sink = atomic_load_explicit(field, memory_order_acquire);
  sink = atomic_exchange_explicit(field, 4u, memory_order_acq_rel);
}

[[noreturn]] void abi_atomic_probe_start(void);

[[noreturn]] void abi_atomic_probe_start(void) {
  touch_u64(&status_page.header.seq);
  touch_u32(&spec_page.header.active_bank);
  touch_u32(&spec_page.banks[0].bank_seq);
  touch_u64(&identity_page.header.completion_ack);
  touch_u64(&worker_status_page.header.request_ack);
  touch_u64(&worker_status_page.header.status_seq);
  touch_u64(&journal_page.header.published_seq);
  for (;;) {
  }
}
