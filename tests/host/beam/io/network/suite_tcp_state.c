/* runtime_tcp_state.h: index validation, the socket lifecycle transition
 * table, the per-syscall gates, readiness, and reference counting. */
#include "check.h"

#include "runtime_tcp_state.h"

#include <limits.h>
#include <string.h>

/* Every state, in ordinal order, so a sweep covers the whole enum. */
static const socket_state_t all_states[] = {
    socket_state_unallocated,    socket_state_allocated, socket_state_bound,
    socket_state_connecting,     socket_state_connected, socket_state_closing,
    socket_state_closed_by_peer, socket_state_error,     socket_state_listening,
};

enum : size_t { state_count = sizeof(all_states) / sizeof(all_states[0]) };

static void test_state_ordinals(void) {
  /* The ordinals are what TCP_DEBUG traces print, so they are part of the
   * file's observable output and must not drift. */
  CHECK(socket_state_unallocated == 0);
  CHECK(socket_state_allocated == 1);
  CHECK(socket_state_bound == 2);
  CHECK(socket_state_connecting == 3);
  CHECK(socket_state_connected == 4);
  CHECK(socket_state_closing == 5);
  CHECK(socket_state_closed_by_peer == 6);
  CHECK(socket_state_error == 7);
  CHECK(socket_state_listening == 8);
  CHECK_EQ_U64(state_count, 9);

  /* Every state names itself, and nothing else does. */
  for (size_t i = 0; i < state_count; i++) {
    const char *name = socket_state_name(all_states[i]);
    CHECK(strcmp(name, "unknown") != 0);
    for (size_t j = 0; j < i; j++) {
      CHECK(strcmp(name, socket_state_name(all_states[j])) != 0);
    }
  }
  CHECK(strcmp(socket_state_name((socket_state_t)42), "unknown") == 0);
}

static void test_index_valid(void) {
  CHECK(!tcp_index_valid(-1, 10));
  CHECK(tcp_index_valid(0, 10));
  CHECK(tcp_index_valid(9, 10));
  CHECK(!tcp_index_valid(10, 10));
  CHECK(!tcp_index_valid(11, 10));
  CHECK(!tcp_index_valid(INT_MIN, 10));
  CHECK(!tcp_index_valid(INT_MAX, 10));
  /* An empty table accepts nothing, including zero. */
  CHECK(!tcp_index_valid(0, 0));
  CHECK(!tcp_index_valid(0, -1));
}

/*
 * The allowed edges, written out independently of the implementation so the
 * table is checked against a second statement of the same rule rather than
 * against itself.
 */
typedef struct {
  socket_state_t from;
  socket_state_t to;
} edge;

static const edge allowed_edges[] = {
    {socket_state_unallocated, socket_state_allocated},

    {socket_state_allocated, socket_state_bound},
    {socket_state_allocated, socket_state_connecting},
    {socket_state_allocated, socket_state_connected},
    {socket_state_allocated, socket_state_listening},
    {socket_state_allocated, socket_state_unallocated},
    {socket_state_allocated, socket_state_error},

    {socket_state_bound, socket_state_connecting},
    {socket_state_bound, socket_state_listening},
    {socket_state_bound, socket_state_unallocated},
    {socket_state_bound, socket_state_error},

    {socket_state_connecting, socket_state_connected},
    {socket_state_connecting, socket_state_unallocated},
    {socket_state_connecting, socket_state_error},

    {socket_state_connected, socket_state_closed_by_peer},
    {socket_state_connected, socket_state_closing},
    {socket_state_connected, socket_state_unallocated},
    {socket_state_connected, socket_state_error},

    {socket_state_closing, socket_state_unallocated},
    {socket_state_closing, socket_state_error},

    {socket_state_closed_by_peer, socket_state_unallocated},
    {socket_state_closed_by_peer, socket_state_error},

    {socket_state_error, socket_state_unallocated},

    {socket_state_listening, socket_state_closing},
    {socket_state_listening, socket_state_unallocated},
    {socket_state_listening, socket_state_error},
};

enum : size_t {
  allowed_count = sizeof(allowed_edges) / sizeof(allowed_edges[0])
};

static bool edge_in_table(socket_state_t from, socket_state_t to) {
  for (size_t i = 0; i < allowed_count; i++) {
    if (allowed_edges[i].from == from && allowed_edges[i].to == to) {
      return true;
    }
  }
  return false;
}

/* The whole square, both directions, against the independent list. */
static void test_transitions_exhaustive(void) {
  for (size_t i = 0; i < state_count; i++) {
    for (size_t j = 0; j < state_count; j++) {
      const socket_state_t from = all_states[i];
      const socket_state_t to = all_states[j];
      CHECK(tcp_state_transition_allowed(from, to) == edge_in_table(from, to));
    }
  }
}

/* Structural properties the table must keep, stated as rules rather than as
 * individual edges, so a future edit that breaks the shape is caught. */
static void test_transition_shape(void) {
  for (size_t i = 0; i < state_count; i++) {
    const socket_state_t s = all_states[i];

    /* No state transitions to itself: tcp.c's setter would then hide a
     * repeated event rather than report it. */
    CHECK(!tcp_state_transition_allowed(s, s));

    /* Nothing is ever reachable from a free slot except allocation. */
    if (s != socket_state_allocated) {
      CHECK(!tcp_state_transition_allowed(socket_state_unallocated, s));
    }

    /* The lwIP error callback fires on any socket that still owns a PCB, so
     * every state but the two that own none reaches error. */
    const bool owns_pcb =
        s != socket_state_unallocated && s != socket_state_error;
    CHECK(tcp_state_transition_allowed(s, socket_state_error) == owns_pcb);

    /* Every state that owns resources can be released directly. A graceful
     * close goes through closing first, but the direct edge has to exist:
     * the accept backlog tears its sockets down without one. */
    if (s != socket_state_unallocated) {
      CHECK(tcp_state_transition_allowed(s, socket_state_unallocated));
    }
  }

  /* An out-of-range state is inert in both directions. */
  const socket_state_t bogus = (socket_state_t)42;
  for (size_t i = 0; i < state_count; i++) {
    CHECK(!tcp_state_transition_allowed(bogus, all_states[i]));
    CHECK(!tcp_state_transition_allowed(all_states[i], bogus));
  }
}

static void test_connect_gate(void) {
  CHECK(tcp_connect_gate(socket_state_allocated) == tcp_connect_start);
  CHECK(tcp_connect_gate(socket_state_bound) == tcp_connect_start);
  CHECK(tcp_connect_gate(socket_state_connected) ==
        tcp_connect_already_connected);
  CHECK(tcp_connect_gate(socket_state_connecting) == tcp_connect_in_progress);
  CHECK(tcp_connect_gate(socket_state_unallocated) == tcp_connect_invalid);
  CHECK(tcp_connect_gate(socket_state_closing) == tcp_connect_invalid);
  CHECK(tcp_connect_gate(socket_state_closed_by_peer) == tcp_connect_invalid);
  CHECK(tcp_connect_gate(socket_state_error) == tcp_connect_invalid);
  CHECK(tcp_connect_gate(socket_state_listening) == tcp_connect_invalid);
  CHECK(tcp_connect_gate((socket_state_t)42) == tcp_connect_invalid);

  /* A connect may only start where the transition table agrees. */
  for (size_t i = 0; i < state_count; i++) {
    const socket_state_t s = all_states[i];
    if (tcp_connect_gate(s) == tcp_connect_start) {
      CHECK(tcp_state_transition_allowed(s, socket_state_connecting));
    }
  }
}

static void test_write_gate(void) {
  /* Only connected and closed_by_peer send, whichever the blocking mode. */
  for (size_t i = 0; i < state_count; i++) {
    const socket_state_t s = all_states[i];
    const bool sends =
        s == socket_state_connected || s == socket_state_closed_by_peer;
    CHECK((tcp_write_gate(s, false) == tcp_write_send) == sends);
    CHECK((tcp_write_gate(s, true) == tcp_write_send) == sends);
  }

  /* The half close keeps the send direction open. */
  CHECK(tcp_write_gate(socket_state_closed_by_peer, false) == tcp_write_send);
  CHECK(tcp_write_gate(socket_state_closed_by_peer, true) == tcp_write_send);

  /* A non-blocking write while the connect resolves is EAGAIN, and only
   * then: the blocking caller gets ENOTCONN, as upstream does. */
  CHECK(tcp_write_gate(socket_state_connecting, true) == tcp_write_would_block);
  CHECK(tcp_write_gate(socket_state_connecting, false) ==
        tcp_write_not_connected);
  for (size_t i = 0; i < state_count; i++) {
    const socket_state_t s = all_states[i];
    if (s != socket_state_connecting) {
      CHECK(tcp_write_gate(s, true) != tcp_write_would_block);
    }
  }

  /* A failed socket reports its recorded error rather than ENOTCONN. */
  CHECK(tcp_write_gate(socket_state_error, false) == tcp_write_pending_error);
  CHECK(tcp_write_gate(socket_state_error, true) == tcp_write_pending_error);

  CHECK(tcp_write_gate(socket_state_unallocated, false) ==
        tcp_write_not_connected);
  CHECK(tcp_write_gate(socket_state_allocated, false) ==
        tcp_write_not_connected);
  CHECK(tcp_write_gate(socket_state_bound, false) == tcp_write_not_connected);
  CHECK(tcp_write_gate(socket_state_closing, false) == tcp_write_not_connected);
  CHECK(tcp_write_gate(socket_state_listening, false) ==
        tcp_write_not_connected);
  CHECK(tcp_write_gate((socket_state_t)42, false) == tcp_write_not_connected);
}

static void test_recv_gate(void) {
  /* Only connected and closed_by_peer receive, in any of the four modes. */
  for (size_t i = 0; i < state_count; i++) {
    const socket_state_t s = all_states[i];
    const bool receives =
        s == socket_state_connected || s == socket_state_closed_by_peer;
    for (int buffered = 0; buffered < 2; buffered++) {
      for (int nonblock = 0; nonblock < 2; nonblock++) {
        const tcp_recv_gate_t gate =
            tcp_recv_gate(s, buffered != 0, nonblock != 0);
        CHECK((gate != tcp_recv_not_connected) == receives);
        /* Buffered bytes are always copied out first, whatever the state
         * and whatever the blocking mode. */
        if (receives && buffered) {
          CHECK(gate == tcp_recv_copy);
        }
      }
    }
  }

  /* Draining order after the peer's FIN: buffered data, then EOF. */
  CHECK(tcp_recv_gate(socket_state_closed_by_peer, true, true) ==
        tcp_recv_copy);
  CHECK(tcp_recv_gate(socket_state_closed_by_peer, false, true) ==
        tcp_recv_eof);
  CHECK(tcp_recv_gate(socket_state_closed_by_peer, false, false) ==
        tcp_recv_eof);

  /* An empty ring on a live connection blocks or reports EAGAIN. */
  CHECK(tcp_recv_gate(socket_state_connected, false, true) ==
        tcp_recv_would_block);
  CHECK(tcp_recv_gate(socket_state_connected, false, false) == tcp_recv_block);

  CHECK(tcp_recv_gate(socket_state_error, true, false) ==
        tcp_recv_not_connected);
  CHECK(tcp_recv_gate((socket_state_t)42, true, false) ==
        tcp_recv_not_connected);
}

static void test_readiness(void) {
  /* Writability needs a free transmit buffer, except in the error state,
   * where the point is to hand the caller its pending error. */
  CHECK(tcp_state_writable(socket_state_connected, true));
  CHECK(!tcp_state_writable(socket_state_connected, false));
  CHECK(tcp_state_writable(socket_state_closed_by_peer, true));
  CHECK(!tcp_state_writable(socket_state_closed_by_peer, false));
  CHECK(tcp_state_writable(socket_state_error, true));
  CHECK(tcp_state_writable(socket_state_error, false));

  /* A connecting socket is never writable: reporting it writable told ERTS
   * the connect had succeeded while lwIP was still in SYN_SENT. */
  CHECK(!tcp_state_writable(socket_state_connecting, true));

  CHECK(!tcp_state_writable(socket_state_unallocated, true));
  CHECK(!tcp_state_writable(socket_state_allocated, true));
  CHECK(!tcp_state_writable(socket_state_bound, true));
  CHECK(!tcp_state_writable(socket_state_closing, true));
  CHECK(!tcp_state_writable(socket_state_listening, true));
  CHECK(!tcp_state_writable((socket_state_t)42, true));

  /* Writability and the write gate agree on which states can send. */
  for (size_t i = 0; i < state_count; i++) {
    const socket_state_t s = all_states[i];
    if (tcp_state_writable(s, true) && s != socket_state_error) {
      CHECK(tcp_write_gate(s, true) == tcp_write_send);
    }
  }

  /* Hangup is exactly the half close. */
  for (size_t i = 0; i < state_count; i++) {
    CHECK(tcp_state_hup(all_states[i]) ==
          (all_states[i] == socket_state_closed_by_peer));
  }
}

static void test_syscall_allowances(void) {
  for (size_t i = 0; i < state_count; i++) {
    const socket_state_t s = all_states[i];
    CHECK(tcp_bind_allowed(s) == (s == socket_state_allocated));
    CHECK(tcp_accept_allowed(s) == (s == socket_state_listening));
    CHECK(tcp_listen_allowed(s) ==
          (s == socket_state_allocated || s == socket_state_bound));
    /* Anything listen accepts must be able to reach the listening state. */
    if (tcp_listen_allowed(s)) {
      CHECK(tcp_state_transition_allowed(s, socket_state_listening));
    }
    /* Anything bind accepts must be able to reach the bound state. */
    if (tcp_bind_allowed(s)) {
      CHECK(tcp_state_transition_allowed(s, socket_state_bound));
    }
  }
  CHECK(!tcp_bind_allowed((socket_state_t)42));
  CHECK(!tcp_listen_allowed((socket_state_t)42));
  CHECK(!tcp_accept_allowed((socket_state_t)42));
}

static void test_refcounts(void) {
  int refs = 0;

  /* Nothing to share, and nothing to release, from zero. */
  CHECK(!tcp_ref_acquire(&refs));
  CHECK(refs == 0);
  CHECK(tcp_ref_release(&refs) == tcp_ref_underflow);
  CHECK(refs == 0);

  /* A negative count is also refused, and left untouched. */
  refs = -3;
  CHECK(!tcp_ref_acquire(&refs));
  CHECK(refs == -3);
  CHECK(tcp_ref_release(&refs) == tcp_ref_underflow);
  CHECK(refs == -3);

  /* The ordinary dup and close sequence. */
  refs = 1;
  CHECK(tcp_ref_acquire(&refs));
  CHECK(refs == 2);
  CHECK(tcp_ref_acquire(&refs));
  CHECK(refs == 3);
  CHECK(tcp_ref_release(&refs) == tcp_ref_shared);
  CHECK(refs == 2);
  CHECK(tcp_ref_release(&refs) == tcp_ref_shared);
  CHECK(refs == 1);
  /* Only the last release reports the teardown, and it reports it once. */
  CHECK(tcp_ref_release(&refs) == tcp_ref_last);
  CHECK(refs == 0);
  CHECK(tcp_ref_release(&refs) == tcp_ref_underflow);
  CHECK(refs == 0);

  /* Over a long interleaving the count never goes negative and the teardown
   * is reported exactly as often as the count returns to zero. */
  refs = 0;
  int live = 0;
  int teardowns = 0;
  for (int step = 0; step < 300; step++) {
    if (step % 3 == 0) {
      if (live == 0) {
        refs = 1;
        live = 1;
      } else if (tcp_ref_acquire(&refs)) {
        live++;
      }
    } else {
      const tcp_ref_release_t r = tcp_ref_release(&refs);
      if (r == tcp_ref_underflow) {
        CHECK(live == 0);
      } else {
        live--;
        if (r == tcp_ref_last) {
          teardowns++;
          CHECK(live == 0);
        }
      }
    }
    CHECK(refs == live);
    CHECK(refs >= 0);
  }
  CHECK(teardowns > 10);
}

static void test_close_actions(void) {
  CHECK(tcp_close_action(socket_state_listening) == tcp_close_begin);
  CHECK(tcp_close_action(socket_state_connected) == tcp_close_begin);
  CHECK(tcp_close_action(socket_state_connecting) ==
        tcp_close_abort_connecting);
  CHECK(tcp_close_action(socket_state_closed_by_peer) ==
        tcp_close_release_peer_closed);
  CHECK(tcp_close_action(socket_state_allocated) == tcp_close_release_idle);
  CHECK(tcp_close_action(socket_state_bound) == tcp_close_release_idle);
  CHECK(tcp_close_action(socket_state_error) == tcp_close_release_idle);
  CHECK(tcp_close_action(socket_state_unallocated) == tcp_close_invalid);
  CHECK(tcp_close_action(socket_state_closing) == tcp_close_invalid);
  CHECK(tcp_close_action((socket_state_t)42) == tcp_close_invalid);

  /* Every action a close can take must land on a transition the table
   * permits, so close and the lifecycle cannot disagree. */
  for (size_t i = 0; i < state_count; i++) {
    const socket_state_t s = all_states[i];
    switch (tcp_close_action(s)) {
    case tcp_close_begin:
      CHECK(tcp_state_transition_allowed(s, socket_state_closing));
      break;
    case tcp_close_abort_connecting:
    case tcp_close_release_peer_closed:
    case tcp_close_release_idle:
      CHECK(tcp_state_transition_allowed(s, socket_state_unallocated));
      break;
    case tcp_close_invalid:
      break;
    }
  }
}

int main(void) {
  test_state_ordinals();
  test_index_valid();
  test_transitions_exhaustive();
  test_transition_shape();
  test_connect_gate();
  test_write_gate();
  test_recv_gate();
  test_readiness();
  test_syscall_allowances();
  test_refcounts();
  test_close_actions();
  return check_finish("tcp_state");
}
