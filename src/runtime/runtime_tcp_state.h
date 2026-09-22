#ifndef CHRYSOPOLIS_RUNTIME_TCP_STATE_H
#define CHRYSOPOLIS_RUNTIME_TCP_STATE_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The socket lifecycle for the vendored LionsOS socket backend (tcp.c): what
 * state a socket may move to, what each POSIX entry point owes a caller in a
 * given state, and how references are taken and dropped.
 *
 * Header-only and pure: tcp.c keeps its lwIP calls, its semaphores and its
 * ownership transfers, and reads its decisions from here. Every function
 * answers with a typed verdict rather than an errno, so the mapping to
 * -errno stays in tcp.c next to the call it returns from.
 *
 * Names follow tcp.c's upstream spelling so its call sites stay easy to
 * compare with LionsOS lib/sock/tcp.c.
 */

/*
 * The ordinals are upstream's and are printed verbatim by tcp.c's TCP_DEBUG
 * trace, so a trace captured from an older build still reads correctly. Do not
 * renumber; append instead.
 */
typedef enum {
  socket_state_unallocated,
  socket_state_allocated,
  socket_state_bound,
  socket_state_connecting,
  socket_state_connected,
  socket_state_closing,
  socket_state_closed_by_peer,
  socket_state_error,
  socket_state_listening,
} socket_state_t;

/* Name for a state, for tracing and for the unexpected-transition log. */
[[__nodiscard__]] static inline const char *
socket_state_name(socket_state_t state) {
  switch (state) {
  case socket_state_unallocated:
    return "unallocated";
  case socket_state_allocated:
    return "allocated";
  case socket_state_bound:
    return "bound";
  case socket_state_connecting:
    return "connecting";
  case socket_state_connected:
    return "connected";
  case socket_state_closing:
    return "closing";
  case socket_state_closed_by_peer:
    return "closed-by-peer";
  case socket_state_error:
    return "error";
  case socket_state_listening:
    return "listening";
  }
  return "unknown";
}

/*
 * True when index addresses one of count socket slots. Every entry point in
 * tcp.c passes its raw index through this first: the libc socket layer stores
 * -1 in its fd table for a descriptor that is not a socket and hands that
 * value straight to the read, write and close hooks without checking it, so a
 * negative index reaches this backend in ordinary operation.
 */
[[__nodiscard__]] static inline bool tcp_index_valid(int index, int count) {
  return index >= 0 && index < count;
}

/*
 * The socket lifecycle, as the edges tcp.c is allowed to take.
 *
 *   unallocated -> allocated                      socket()
 *   allocated   -> bound                          bind()
 *   allocated   -> connecting                     connect()
 *   allocated   -> connected                      the accept callback, which
 *                                                 pre-allocates a slot and
 *                                                 wires it up before queueing
 *   allocated   -> listening                      listen() without a prior
 *                                                 bind(), which lwIP binds to
 *                                                 any address the way POSIX's
 *                                                 implicit bind does
 *   allocated   -> unallocated                    close(), or the accept
 *                                                 callback undoing itself
 *   bound       -> connecting                     connect()
 *   bound       -> listening                      listen()
 *   bound       -> unallocated                    close()
 *   connecting  -> connected                      the connected callback
 *   connecting  -> unallocated                    close() aborts the PCB
 *   connected   -> closed_by_peer                 FIN, a half close: our send
 *                                                 direction stays open
 *   connected   -> closing                        close() starts lwIP's close
 *   connected   -> unallocated                    a connection the accept
 *                                                 callback wired up but no
 *                                                 accept() ever claimed, torn
 *                                                 down directly because the
 *                                                 application never saw it
 *   closed_by_peer -> unallocated                 close()
 *   closing     -> unallocated                    lwIP reports the close done
 *   listening   -> closing                        close()
 *   error       -> unallocated                    close()
 *   <any live>  -> error                          the lwIP error callback,
 *                                                 which can fire at any point
 *                                                 a PCB is live
 *
 * A socket in error or unallocated takes no further edge except the one shown.
 * tcp.c routes every write to the state field through one setter that checks
 * this, so an edge that is not here is reported rather than silently taken.
 */
[[__nodiscard__]] static inline bool
tcp_state_transition_allowed(socket_state_t from, socket_state_t to) {
  /* The error callback fires on any socket that still owns a live PCB. */
  if (to == socket_state_error) {
    return from == socket_state_allocated || from == socket_state_bound ||
           from == socket_state_connecting || from == socket_state_connected ||
           from == socket_state_closing ||
           from == socket_state_closed_by_peer ||
           from == socket_state_listening;
  }

  switch (from) {
  case socket_state_unallocated:
    return to == socket_state_allocated;
  case socket_state_allocated:
    return to == socket_state_bound || to == socket_state_connecting ||
           to == socket_state_connected || to == socket_state_listening ||
           to == socket_state_unallocated;
  case socket_state_bound:
    return to == socket_state_connecting || to == socket_state_listening ||
           to == socket_state_unallocated;
  case socket_state_connecting:
    return to == socket_state_connected || to == socket_state_unallocated;
  case socket_state_connected:
    return to == socket_state_closed_by_peer || to == socket_state_closing ||
           to == socket_state_unallocated;
  case socket_state_closing:
    return to == socket_state_unallocated;
  case socket_state_closed_by_peer:
    return to == socket_state_unallocated;
  case socket_state_error:
    return to == socket_state_unallocated;
  case socket_state_listening:
    return to == socket_state_closing || to == socket_state_unallocated;
  }
  return false;
}

/* What connect(2) must do in this state. */
typedef enum : uint8_t {
  /* Not connected yet and the PCB is usable: issue lwIP's connect. */
  tcp_connect_start,
  /* Already connected: EISCONN. */
  tcp_connect_already_connected,
  /* A connect is already under way: EALREADY. */
  tcp_connect_in_progress,
  /* Any other state: EINVAL. */
  tcp_connect_invalid,
} tcp_connect_gate_t;

[[__nodiscard__]] static inline tcp_connect_gate_t
tcp_connect_gate(socket_state_t state) {
  switch (state) {
  case socket_state_connected:
    return tcp_connect_already_connected;
  case socket_state_connecting:
    return tcp_connect_in_progress;
  case socket_state_allocated:
  case socket_state_bound:
    return tcp_connect_start;
  case socket_state_unallocated:
  case socket_state_closing:
  case socket_state_closed_by_peer:
  case socket_state_error:
  case socket_state_listening:
    break;
  }
  return tcp_connect_invalid;
}

/*
 * True when listen(2) may be issued. The socket must own a PCB that is not
 * already part of a connection. An unbound socket is allowed: lwIP binds it to
 * any address, which is what POSIX's implicit bind does, and upstream relies
 * on that.
 */
[[__nodiscard__]] static inline bool tcp_listen_allowed(socket_state_t state) {
  return state == socket_state_allocated || state == socket_state_bound;
}

/* True when bind(2) may be issued. */
[[__nodiscard__]] static inline bool tcp_bind_allowed(socket_state_t state) {
  return state == socket_state_allocated;
}

/* True when accept(2) may be issued: only a listening socket has a backlog. */
[[__nodiscard__]] static inline bool tcp_accept_allowed(socket_state_t state) {
  return state == socket_state_listening;
}

/* What write(2) must do in this state. */
typedef enum : uint8_t {
  /* Send direction is open: go on to the transmit buffer check. */
  tcp_write_send,
  /* A non-blocking write while the connect is still resolving: EAGAIN. */
  tcp_write_would_block,
  /* The socket has failed: report its recorded error. */
  tcp_write_pending_error,
  /* No send direction: ENOTCONN. */
  tcp_write_not_connected,
} tcp_write_gate_t;

/*
 * closed_by_peer still sends: the peer's FIN closed its send direction, not
 * ours, and an echo server's reply is written after it arrives.
 *
 * A blocking write on a connecting socket is not handled here. It falls
 * through to not_connected, which is what upstream does, because the socket
 * has no send buffer to wait on until the connect resolves.
 */
[[__nodiscard__]] static inline tcp_write_gate_t
tcp_write_gate(socket_state_t state, bool nonblock) {
  if (state == socket_state_connecting && nonblock) {
    return tcp_write_would_block;
  }
  if (state == socket_state_connected || state == socket_state_closed_by_peer) {
    return tcp_write_send;
  }
  if (state == socket_state_error) {
    return tcp_write_pending_error;
  }
  return tcp_write_not_connected;
}

/* What recv(2) must do in this state. */
typedef enum : uint8_t {
  /* Buffered bytes are waiting: copy them out. */
  tcp_recv_copy,
  /* Peer closed and the ring is drained: return 0. */
  tcp_recv_eof,
  /* Nothing buffered on a non-blocking socket: EAGAIN. */
  tcp_recv_would_block,
  /* Nothing buffered on a blocking socket: park on the receive semaphore. */
  tcp_recv_block,
  /* No receive direction: ENOTCONN. */
  tcp_recv_not_connected,
} tcp_recv_gate_t;

/*
 * buffered is whether the receive ring holds any bytes. Those bytes are
 * readable even after the peer has closed, which is the whole point of the
 * closed_by_peer state: EOF is reported only once the ring is drained.
 */
[[__nodiscard__]] static inline tcp_recv_gate_t
tcp_recv_gate(socket_state_t state, bool buffered, bool nonblock) {
  if (state != socket_state_connected && state != socket_state_closed_by_peer) {
    return tcp_recv_not_connected;
  }
  if (buffered) {
    return tcp_recv_copy;
  }
  if (state == socket_state_closed_by_peer) {
    return tcp_recv_eof;
  }
  return nonblock ? tcp_recv_would_block : tcp_recv_block;
}

/*
 * Whether poll or select should report the socket ready for writing.
 *
 * The socket's state decides this, not just the transmit pool. A connecting
 * socket is the case that matters: POSIX makes it writable exactly when the
 * connect resolves, and that transition is how a non-blocking client learns
 * the connection is up. ERTS's inet_drv is such a client, so reporting a
 * connecting socket as writable told it the connect had succeeded while lwIP
 * was still in SYN_SENT. It then wrote into a socket that could only answer
 * EAGAIN, queued the payload internally, and never re-issued it once the
 * connection actually came up: gen_tcp:send returned ok, the bytes never
 * reached the wire, and because the port stayed open holding them, not even
 * the FIN did. That was a rare race (the connect usually completes first),
 * which is exactly why it surfaced as a CI-only flake in net-restart-smoke.
 *
 * A socket in the error state IS reported writable, deliberately: that is how
 * the caller is told to collect the pending error (a failed connect included)
 * rather than waiting on a socket that will never make progress.
 *
 * tx_pool_free is whether the sDDF transmit pool has a free buffer. It only
 * matters once the state allows a write at all: with no free buffer the write
 * would fail, so the socket is not ready.
 */
[[__nodiscard__]] static inline bool tcp_state_writable(socket_state_t state,
                                                        bool tx_pool_free) {
  switch (state) {
  case socket_state_connected:
  case socket_state_closed_by_peer:
    return tx_pool_free;
  case socket_state_error:
    return true;
  case socket_state_unallocated:
  case socket_state_allocated:
  case socket_state_bound:
  case socket_state_connecting:
  case socket_state_closing:
  case socket_state_listening:
    break;
  }
  return false;
}

/* Whether poll should report a hangup: the peer closed its send direction. */
[[__nodiscard__]] static inline bool tcp_state_hup(socket_state_t state) {
  return state == socket_state_closed_by_peer;
}

/*
 * Socket references. A socket slot is shared by every descriptor dup()ed from
 * it, and only the last close tears down the connection. The count is owned by
 * the caller; these helpers make the 1 to 0 edge and the underflow explicit
 * rather than leaving them to an assert, which is compiled out in this tree.
 */

typedef enum : uint8_t {
  /* Other descriptors still hold this socket: close nothing. */
  tcp_ref_shared,
  /* The last reference went away: run the teardown. */
  tcp_ref_last,
  /* A release with no reference outstanding. Nothing was written. */
  tcp_ref_underflow,
} tcp_ref_release_t;

/* Take a reference. False when there is none to share, leaving *refs alone. */
[[__nodiscard__]] static inline bool tcp_ref_acquire(int *refs) {
  if (*refs <= 0) {
    return false;
  }
  *refs += 1;
  return true;
}

/* Drop a reference. *refs is left alone on underflow. */
[[__nodiscard__]] static inline tcp_ref_release_t tcp_ref_release(int *refs) {
  if (*refs <= 0) {
    return tcp_ref_underflow;
  }
  *refs -= 1;
  return *refs == 0 ? tcp_ref_last : tcp_ref_shared;
}

/* What close(2) must do for the last reference to a socket. */
typedef enum : uint8_t {
  /* Listening or connected: start lwIP's close, the PCB is released later. */
  tcp_close_begin,
  /* Connecting: abort the PCB and release the socket now. */
  tcp_close_abort_connecting,
  /* Peer already closed: detach callbacks, close or abort, release now. */
  tcp_close_release_peer_closed,
  /* No live connection (allocated, bound or failed): release now. */
  tcp_close_release_idle,
  /* Already unallocated or closing: a caller bug. */
  tcp_close_invalid,
} tcp_close_action_t;

[[__nodiscard__]] static inline tcp_close_action_t
tcp_close_action(socket_state_t state) {
  switch (state) {
  case socket_state_listening:
  case socket_state_connected:
    return tcp_close_begin;
  case socket_state_connecting:
    return tcp_close_abort_connecting;
  case socket_state_closed_by_peer:
    return tcp_close_release_peer_closed;
  case socket_state_allocated:
  case socket_state_bound:
  case socket_state_error:
    return tcp_close_release_idle;
  case socket_state_unallocated:
  case socket_state_closing:
    break;
  }
  return tcp_close_invalid;
}

#endif
