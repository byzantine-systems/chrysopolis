/*
 * Copyright 2023, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <lions/posix/posix.h>
#include <microkit.h>
#include <libmicrokitco.h>

#include "runtime_network.h"
#include "runtime_tcp_logic.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>

#include <sddf/timer/client.h>
#include <sddf/timer/config.h>
#include <sddf/network/lib_sddf_lwip.h>
#include <sddf/network/queue.h>
#include <sddf/network/util.h>
#include <sddf/network/constants.h>
#include <sddf/network/config.h>

#include <lwip/dhcp.h>
#include <lwip/init.h>
#include <lwip/ip.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/snmp.h>
#include <lwip/stats.h>
#include <lwip/sys.h>
#include <lwip/tcp.h>
#include <lwip/timeouts.h>
#include <netif/etharp.h>

#include <lions/util.h>

#define SOCK_SUCC 0
#define SOCK_ERR  1

#define MAX_SOCKETS 10
#define MAX_LISTEN_BACKLOG 10
#define SOCKET_BUF_SIZE 0x200000ll

static int socket_refcount[MAX_SOCKETS];

static ssize_t lwip_err_to_errno[20] = {
    [ERR_OK] = 0,

    //lwIP errors are negative
    [-ERR_MEM] = ENOMEM,
    [-ERR_BUF] = ENOBUFS,
    [-ERR_TIMEOUT] = ETIMEDOUT,
    [-ERR_RTE] = EHOSTUNREACH,
    [-ERR_INPROGRESS] = EINPROGRESS,
    [-ERR_VAL] = EINVAL,
    [-ERR_WOULDBLOCK] = EAGAIN,
    [-ERR_USE] = EADDRINUSE,
    [-ERR_ALREADY] = EALREADY,
    [-ERR_ISCONN] = EISCONN,
    [-ERR_CONN] = ENOTCONN,
    [-ERR_IF] = ENODEV,
    [-ERR_ABRT] = ECONNABORTED,
    [-ERR_RST] = ECONNRESET,
    [-ERR_CLSD] = ENOTCONN,
    [-ERR_ARG] = EINVAL,
};

/*
 * errno for an lwIP result. An err_t the table has no entry for becomes EIO:
 * answering 0 there would turn an unrecognised failure into a reported
 * success, which the caller has no way to tell from a real one.
 */
static ssize_t lwip_errno(err_t err) {
    if (err == ERR_OK) {
        return 0;
    }
    if (!tcp_lwip_err_in_table(err, sizeof(lwip_err_to_errno) / sizeof(lwip_err_to_errno[0]))) {
        return EIO;
    }
    ssize_t mapped = lwip_err_to_errno[-err];
    return mapped != 0 ? mapped : EIO;
}

typedef struct {
    int pending_socket_indices[MAX_LISTEN_BACKLOG];
    int head;
    int tail;
    microkit_cothread_sem_t accept_sem;
} accept_queue_t;

typedef struct {
    struct tcp_pcb *sock_tpcb;
    socket_state_t state;
    int last_error;

    char rx_buf[SOCKET_BUF_SIZE];
    /* Unsigned: every ring helper works in size_t, and a negative head or
     * length has no meaning. */
    size_t rx_head;
    size_t rx_len;

    accept_queue_t accept_queue;
    microkit_cothread_sem_t connect_sem;
    microkit_cothread_sem_t recv_sem;
    microkit_cothread_sem_t send_sem;
} socket_t;

/* tcp_socket_readable reports the buffered byte count through an int. */
static_assert(SOCKET_BUF_SIZE <= INT_MAX, "receive ring must fit an int");

static socket_t sockets[MAX_SOCKETS] = { 0 };

static int socket_id(socket_t *socket) { return (int)(socket - sockets); }

/*
 * Socket-state tracing, compiled out unless built with `-Dtcp-debug=true`,
 * which is what makes build.zig define TCP_DEBUG for this file.
 *
 * It exists because the failures this layer produces are races between ERTS's
 * non-blocking socket usage and lwIP's connection state, and those are
 * invisible from the console: the guest reports a successful send while the
 * wire carries nothing. Tracing every state transition here is what turns that
 * into a readable sequence. Kept compiled-in-able rather than commented out so
 * the next investigation starts from a flag rather than from re-deriving which
 * prints were useful.
 *
 * microkit_dbg_puts, NEVER printf: printf goes through the serial driver at
 * about a second per write under TCG, which rewrites the timing of the very
 * race being traced. dbg_puts is a direct kernel putchar.
 *
 * Format is one line per event, `TCP|<event>|sock=<n>|state=<n>|v=<n>`, with
 * `v` an event-specific number (a port, a length, an errno). state is the
 * socket_state_t ordinal.
 */
#ifndef TCP_DEBUG
#define TCP_DEBUG 0
#endif

#if TCP_DEBUG
static void tcp_trace_dec(long v) {
    char buf[24];
    unsigned int i = sizeof(buf);
    unsigned long mag = (v < 0) ? (unsigned long)-v : (unsigned long)v;
    buf[--i] = '\0';
    do {
        buf[--i] = (char)('0' + (mag % 10));
        mag /= 10;
    } while (mag);
    if (v < 0) {
        buf[--i] = '-';
    }
    microkit_dbg_puts(&buf[i]);
}

static void tcp_trace(const char *event, int index, socket_state_t state, long value) {
    microkit_dbg_puts("TCP|");
    microkit_dbg_puts(event);
    microkit_dbg_puts("|sock=");
    tcp_trace_dec(index);
    microkit_dbg_puts("|state=");
    tcp_trace_dec((long)state);
    microkit_dbg_puts("|v=");
    tcp_trace_dec(value);
    microkit_dbg_puts("\n");
}
#else
/* Empty in a non-debug build, NOT a macro: the call sites stay type-checked
 * whichever way TCP_DEBUG is set, so a trace that has gone stale fails the
 * ordinary build rather than only the rare debug one. Every argument here is a
 * plain value, so the compiler drops the call entirely at -O1 and above. */
static inline void tcp_trace([[maybe_unused]] const char *event, [[maybe_unused]] int index,
                             [[maybe_unused]] socket_state_t state, [[maybe_unused]] long value) {}
#endif

/* DIAG: wall-clock ms since boot for latency tracing. Kept (with the
 * commented-out DIAG printfs below) for future debugging, uncomment as
 * needed. */
// static unsigned long diag_ms(void) {
//     return (unsigned long)(sddf_timer_time_now(timer_config.driver_id) / 1000000ull);
// }

/*
 * Ownership contract for this file. Each resource below has exactly one owner
 * at a time and exactly one release, and the helpers underneath this comment
 * are the only places those transfers happen.
 *
 * PCB. Created by tcp_new_ip_type on the connect path, or handed to
 *   tcp_socket_accept_cb by lwIP on the accept path. Exactly one socket_t owns
 *   it through sock_tpcb, and exactly one of tcp_close, tcp_abort, or lwIP's
 *   own free just before it calls socket_err_func releases it. sock_tpcb is
 *   NULL whenever the socket owns no PCB, so a released PCB is never reachable
 *   from here. One case belongs to lwIP rather than to us: when the accept
 *   callback answers anything but ERR_OK, lwIP aborts the new PCB itself, so
 *   that path must not close it as well.
 *
 * pbuf. socket_recv_callback either takes the pbuf, copying it into the ring
 *   and calling pbuf_free, and answers ERR_OK; or refuses it, freeing nothing
 *   and answering ERR_MEM so lwIP keeps it and delivers it again. Never both.
 *
 * Receive ring. rx_head and rx_len belong to the slot. The receive callback is
 *   the only producer, tcp_socket_recv the only consumer, and socket_clear the
 *   only reset.
 *
 * Backlog entry. An index queued by tcp_socket_accept_cb hands a fully wired
 *   socket to the listening socket's queue. Exactly one tcp_socket_accept
 *   claims it, or socket_drain_backlog releases it when the listening socket
 *   closes. Nothing else can: the application never saw those indices.
 *
 * Reference count. Set to 1 by tcp_socket_init and by a successful
 *   tcp_socket_accept, raised by tcp_socket_dup, lowered by tcp_socket_close.
 *   Only the drop to zero runs tcp_socket_close_int.
 */

/*
 * Resolve an index from the libc socket layer. That layer keeps -1 in its fd
 * table for a descriptor that is not a socket and hands it to the read, write
 * and close hooks without checking it, so an out-of-range index arrives here
 * in ordinary operation and must not reach sockets[].
 */
static socket_t *socket_lookup(int index) {
    return tcp_index_valid(index, MAX_SOCKETS) ? &sockets[index] : NULL;
}

/* Signal a semaphore only when a cothread is parked on it, so an event with
 * no waiter does not leave a count for the next one to consume. */
static void socket_wake(microkit_cothread_sem_t *sem) {
    if (!microkit_cothread_semaphore_is_queue_empty(sem)) {
        microkit_cothread_semaphore_signal(sem);
    }
}

/*
 * The only writer of socket->state. An edge the lifecycle does not define is
 * reported and then taken: refusing it would leave the socket describing a
 * connection lwIP has already moved past, which is worse than a wrong state we
 * can read in the log. assert() is compiled out in this tree, so this has to
 * be ordinary code. Off the hot path: one branch per connection event, and
 * output only on a transition that should never happen.
 */
static void socket_set_state(socket_t *socket, socket_state_t next) {
    if (!tcp_state_transition_allowed(socket->state, next)) {
        dlog("socket %d: unexpected transition %s -> %s", socket_id(socket),
             socket_state_name(socket->state), socket_state_name(next));
    }
    tcp_trace("state", socket_id(socket), next, (long)socket->state);
    socket->state = next;
}

/*
 * Detach the socket argument and every callback from a PCB the socket is
 * giving up, so a late ACK, a late segment or an abort cannot reach a slot
 * that has been released or reused. The caller then releases the PCB, or
 * leaves it to lwIP where lwIP owns it.
 */
static void socket_detach_pcb(struct tcp_pcb *tpcb) {
    tcp_arg(tpcb, NULL);
    tcp_sent(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_err(tpcb, NULL);
}

/* Release a PCB this socket owns. lwIP cannot always close (it needs memory
 * for the FIN), and an abort always succeeds, so fall back to it. */
static void socket_close_pcb(struct tcp_pcb *tpcb) {
    socket_detach_pcb(tpcb);
    if (tcp_close(tpcb) != ERR_OK) {
        tcp_abort(tpcb);
    }
}

/* Clear everything a later allocation of this slot could observe. The PCB
 * must already be released. */
static void socket_clear(socket_t *socket) {
    socket->sock_tpcb = NULL;
    socket->rx_head = 0;
    socket->rx_len = 0;
    socket->last_error = 0;
    socket->accept_queue.head = 0;
    socket->accept_queue.tail = 0;
    socket_refcount[socket_id(socket)] = 0;
}

/* Return a slot to the free pool. */
static void socket_release(socket_t *socket) {
    socket_set_state(socket, socket_state_unallocated);
    socket_clear(socket);
}

static void socket_err_func(void *arg, err_t err) {
    socket_t *socket = arg;
    if (socket == NULL) {
        dlog("error %d with closed socket", err);
    } else {
        dlog("error %d with socket %d which is in state %d", err, socket_id(socket), socket->state);
        tcp_trace("err", socket_id(socket), socket->state, (long)err);

        socket_set_state(socket, socket_state_error);
        socket->last_error = lwip_errno(err);
        /* lwIP frees the PCB before calling this, so the pointer we hold is
         * already dangling. Drop it: every path from here reads the state
         * first and none may follow sock_tpcb again. */
        socket->sock_tpcb = NULL;

        /* Wake every blocked call, not just connect. Each one re-reads the
         * state and reports the failure instead of parking again: a recv
         * waiting for bytes that will never arrive and a write waiting for
         * send buffer on a connection that is gone would otherwise sleep for
         * the life of the PD. */
        socket_wake(&socket->connect_sem);
        socket_wake(&socket->recv_sem);
        socket_wake(&socket->send_sem);
    }
}

/* tpcb goes unread: the socket reached through arg owns the same PCB, and
 * reading it from there keeps every path in this file going through the slot.
 * The parameter stays because lwIP's tcp_recv_fn signature has it. */
static err_t socket_recv_callback(void *arg, [[maybe_unused]] struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    dlogp(err, "error %d", err);

    socket_t *socket = arg;
    assert(socket != NULL);
    int socket_index = socket_id(socket);
    dlogp(err, "error %d with socket %d", err, socket_index);

    switch (socket->state) {
    case socket_state_connected: {
        // printf("DIAG|%lu|recv_cb sock=%d %s\n", diag_ms(), socket_index,
        //        p ? "data" : "FIN");
        if (p != NULL) {
            if (!tcp_rx_can_accept(socket->rx_len, p->tot_len, SOCKET_BUF_SIZE)) {
                /* Refuse the pbuf without freeing it: ERR_MEM is what tells
                 * lwIP to keep it and deliver it again once the application
                 * has drained the ring. An errno here would be read as some
                 * unrelated lwIP error, and the segment would be lost. */
                return ERR_MEM;
            }

            /* Both counters stay under p->tot_len, which is a u16_t, so the
             * pbuf_copy_partial arguments cannot be truncated. */
            size_t copied = 0, remaining = p->tot_len;
            while (remaining != 0) {
                size_t rx_tail = tcp_rx_tail(socket->rx_head, socket->rx_len, SOCKET_BUF_SIZE);
                size_t to_copy = MIN(remaining, tcp_rx_free_span(socket->rx_head, socket->rx_len, SOCKET_BUF_SIZE));
                pbuf_copy_partial(p, socket->rx_buf + rx_tail, (u16_t)to_copy, (u16_t)copied);
                socket->rx_len += to_copy;
                copied += to_copy;
                remaining -= to_copy;
            }
            pbuf_free(p);
        } else {
            /* FIN from peer: a half-close, not a full close. The peer is done
             * sending, but we may still have data to send (an echo server's
             * reply, e.g.), so keep the PCB open and the callbacks registered.
             *
             * The PCB is closed when the application close()s. */
            socket_set_state(socket, socket_state_closed_by_peer);
        }
        // Wake any blocked recv() call
        socket_wake(&socket->recv_sem);
        return SOCK_SUCC;
    }

    case socket_state_allocated:
    case socket_state_closing: {
        if (p != NULL) {
            pbuf_free(p);
        } else {
            /* lwIP has finished the close this socket started and frees the
             * PCB itself. Detach first so nothing reaches the slot again. */
            tcp_arg(socket->sock_tpcb, NULL);
            socket_release(socket);
        }
        return SOCK_SUCC;
    }

    default:
        dlog("called on invalid socket state: %d (socket=%d)", socket->state, socket_index);
        assert(false);
        if (p != NULL) {
            pbuf_free(p);
        }
        /* SOCK_ERR is +1, which is not a valid err_t: every lwIP error is
         * negative. ERR_ARG says the callback rejected what it was given. */
        return ERR_ARG;
    }
}

/* Only the wake-up matters here: the writer re-reads tcp_sndbuf itself, so
 * neither the PCB nor the acknowledged length is needed. Both parameters stay
 * because lwIP's tcp_sent_fn signature has them. */
static err_t socket_sent_callback(void *arg, [[maybe_unused]] struct tcp_pcb *pcb,
                                  [[maybe_unused]] u16_t len) {
    socket_t *socket = arg;
    if (socket == NULL) {
        /* Late ACK on a detached PCB (closed while data was in flight). */
        return SOCK_SUCC;
    }

    socket_wake(&socket->send_sem);

    return SOCK_SUCC;
}

static err_t socket_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    socket_t *socket = arg;
    assert(socket != NULL);
    if (socket == NULL) {
        /* The socket was released while the connect was in flight, so the
         * close path already owns this PCB. */
        return SOCK_SUCC;
    }

    socket_set_state(socket, socket_state_connected);
    tcp_trace("connected", socket_id(socket), socket->state, (long)err);

    tcp_sent(tpcb, socket_sent_callback);
    tcp_recv(tpcb, socket_recv_callback);

    tpcb->so_options |= SOF_KEEPALIVE;

    // Wake the connect() call
    socket_wake(&socket->connect_sem);

    return SOCK_SUCC;
}

static int socket_allocate() {
    int free_index;
    socket_t *socket = NULL;
    for (free_index = 0; free_index < MAX_SOCKETS; free_index++) {
        if (sockets[free_index].state == socket_state_unallocated) {
            socket = &sockets[free_index];
            break;
        }
    }
    if (socket == NULL) {
        dlog("no free sockets");
        return -ENOMEM;
    }

    /* A free slot is already clear. Clearing it again costs nothing and makes
     * that true even if a teardown path ever misses a field. */
    socket_clear(socket);
    socket_set_state(socket, socket_state_allocated);

    return free_index;
}

static int tcp_socket_init(int index) {
    socket_t *socket = socket_lookup(index);
    if (socket == NULL) {
        return -EBADF;
    }

    if (socket->state != socket_state_allocated) {
        return -EINVAL;
    }

    socket->sock_tpcb = tcp_new_ip_type(IPADDR_TYPE_V4);

    if (socket->sock_tpcb == NULL) {
        dlog("couldn't init socket");
        return -ENOMEM;
    }

    socket->sock_tpcb->so_options |= SOF_KEEPALIVE;

    socket->accept_queue.head = 0;
    socket->accept_queue.tail = 0;
    microkit_cothread_semaphore_init(&socket->accept_queue.accept_sem);
    microkit_cothread_semaphore_init(&socket->connect_sem);
    microkit_cothread_semaphore_init(&socket->recv_sem);
    microkit_cothread_semaphore_init(&socket->send_sem);

    tcp_err(socket->sock_tpcb, socket_err_func);
    tcp_arg(socket->sock_tpcb, socket);

    /* The socket() call that reached here holds the one reference. An
     * increment would compound a count a teardown path had left behind. */
    socket_refcount[index] = 1;

    return SOCK_SUCC;
}

static int tcp_socket_connect(int index, uint32_t addr, uint16_t port, int flags) {
    socket_t *sock = socket_lookup(index);
    if (sock == NULL) {
        return -EBADF;
    }

    switch (tcp_connect_gate(sock->state)) {
    case tcp_connect_start:
        break;
    case tcp_connect_already_connected:
        return -EISCONN;
    case tcp_connect_in_progress:
        return -EALREADY;
    case tcp_connect_invalid:
        return -EINVAL;
    }

    ip_addr_t ipaddr;
    ip4_addr_set_u32(&ipaddr, addr);

    tcp_trace("connect", index, sock->state, port);

    /* The state moves only once lwIP has taken the connect. Neither callback
     * can run before tcp_connect returns, so there is no window here, and a
     * refused connect leaves the socket where it was: still bound, still
     * closable, and able to try again. */
    err_t err = tcp_connect(sock->sock_tpcb, &ipaddr, port, socket_connected);
    if (err != ERR_OK) {
        dlog("error connecting (%d)", err);
        return -lwip_errno(err);
    }
    socket_set_state(sock, socket_state_connecting);

    if (flags & O_NONBLOCK) {
        /* The caller now learns the outcome only by polling, which is where
         * tcp_socket_writable has to be honest about the connection state. */
        tcp_trace("connect-inprogress", index, sock->state, port);
        return -EINPROGRESS;
    }

    // Block until connection established or error occurs
    while (sock->state == socket_state_connecting) {
        microkit_cothread_semaphore_wait(&sock->connect_sem);
    }

    tcp_trace("connect-done", index, sock->state, 0);
    if (sock->state == socket_state_connected) {
        return SOCK_SUCC;
    } else {
        return -ECONNREFUSED;
    }
}

/*
 * Release every socket the accept callback queued that no accept() claimed.
 * Each owns a PCB and carries no reference, and the application never saw its
 * index, so the listening socket's close is the only place they can go.
 */
static void socket_drain_backlog(socket_t *listen_socket) {
    accept_queue_t *q = &listen_socket->accept_queue;
    int pending = 0;

    while (tcp_backlog_pop(q->pending_socket_indices, MAX_LISTEN_BACKLOG, q->head, &q->tail, &pending)) {
        socket_t *socket = socket_lookup(pending);
        if (socket == NULL) {
            continue;
        }
        tcp_trace("backlog-drop", pending, socket->state, 0);
        if (socket->sock_tpcb != NULL) {
            socket_close_pcb(socket->sock_tpcb);
        }
        socket_release(socket);
    }
}

static int tcp_socket_close_int(int index) {
    socket_t *socket = &sockets[index];

    tcp_trace("close", index, socket->state, 0);
    switch (tcp_close_action(socket->state)) {
    case tcp_close_begin: {
        if (socket->state == socket_state_listening) {
            socket_drain_backlog(socket);
        }
        socket_set_state(socket, socket_state_closing);
        int err = tcp_close(socket->sock_tpcb);
        if (err != ERR_OK) {
            dlog("error closing socket (%d)", err);
            return -lwip_errno(err);
        }
        return SOCK_SUCC;
    }

    case tcp_close_abort_connecting: {
        tcp_arg(socket->sock_tpcb, NULL);  // Prevent error callback noise
        tcp_abort(socket->sock_tpcb);
        socket_release(socket);

        return SOCK_SUCC;
    }

    case tcp_close_release_peer_closed: {
        /* Peer already sent FIN but the PCB is still open on our side (we
         * keep it alive for half-close writes). Detach the arg AND the
         * callbacks: ACKs for data we just wrote (an echo reply, e.g.) can
         * still arrive after close and would fire socket_sent_callback with
         * a NULL arg. lwIP flushes queued data before the FIN. Fall back to
         * abort if lwIP can't close (out of memory). */
        socket_close_pcb(socket->sock_tpcb);
        socket_release(socket);

        return SOCK_SUCC;
    }

    case tcp_close_release_idle: {
        /* allocated and bound still own the PCB tcp_new_ip_type handed over,
         * and dropping the slot without releasing it leaked one PCB per
         * socket()/close() pair. The error state owns none: lwIP freed it
         * before calling socket_err_func, which cleared the pointer. */
        if (socket->sock_tpcb != NULL) {
            socket_close_pcb(socket->sock_tpcb);
        }
        socket_release(socket);

        return SOCK_SUCC;
    }

    case tcp_close_invalid:
    default:
        dlog("called on invalid socket state: %d", socket->state);
        assert(false);
        return -EBADF;
    }
}

static int tcp_socket_close(int index) {
    socket_t *socket = socket_lookup(index);
    if (socket == NULL) {
        return -EBADF;
    }

    switch (tcp_ref_release(&socket_refcount[index])) {
    case tcp_ref_last:
        return tcp_socket_close_int(index);

    case tcp_ref_shared:
        return SOCK_SUCC;

    case tcp_ref_underflow:
        break;
    }

    /* No reference to drop. libc reaches here on one legitimate path: it
     * closes the handle when tcp_socket_init fails, before the socket has a
     * reference at all. The slot still has to go back to the free pool. */
    if (socket->state == socket_state_unallocated) {
        dlog("close of socket %d which is not allocated", index);
        return -EBADF;
    }
    return tcp_socket_close_int(index);
}

static int tcp_socket_dup(int index) {
    if (socket_lookup(index) == NULL) {
        return -EBADF;
    }
    if (!tcp_ref_acquire(&socket_refcount[index])) {
        dlog("dup of socket %d which holds no reference", index);
        return -EBADF;
    }
    return SOCK_SUCC;
}

static ssize_t tcp_socket_write(int index, const char *buf, size_t len, int flags) {
    socket_t *sock = socket_lookup(index);
    if (sock == NULL) {
        return -EBADF;
    }
    // printf("DIAG|%lu|write sock=%d len=%zu\n", diag_ms(), index, len);
    const bool nonblock = (flags & O_NONBLOCK) != 0;

    switch (tcp_write_gate(sock->state, nonblock)) {
    case tcp_write_send:
        break;

    case tcp_write_would_block:
        // Write during connection establishment, nonblocking mode
        tcp_trace("write-eagain", index, sock->state, (long)len);
        return -EAGAIN;

    case tcp_write_pending_error:
        tcp_trace("write-notconn", index, sock->state, (long)len);
        return sock->last_error ? -sock->last_error : -ENOTCONN;

    case tcp_write_not_connected:
        tcp_trace("write-notconn", index, sock->state, (long)len);
        return -ENOTCONN;
    }

    if (tcp_sndbuf(sock->sock_tpcb) == 0) {
        if (nonblock) {
            return -EAGAIN;
        }
        /* Block until send buffer available. The gate is read before the PCB
         * on every pass: socket_err_func clears sock_tpcb, so a connection
         * that fails while this cothread is parked must not be followed. */
        while (tcp_write_gate(sock->state, false) == tcp_write_send &&
               tcp_sndbuf(sock->sock_tpcb) == 0) {
            microkit_cothread_semaphore_wait(&sock->send_sem);
        }
        if (tcp_write_gate(sock->state, false) != tcp_write_send) {
            tcp_trace("write-notconn", index, sock->state, (long)len);
            return sock->last_error ? -sock->last_error : -ENOTCONN;
        }
    }

    u16_t to_write = tcp_write_chunk(len, tcp_sndbuf(sock->sock_tpcb));

    err_t err = tcp_write(sock->sock_tpcb, (void *)buf, to_write, 1);
    if (err != ERR_OK) {
        dlog("tcp_write failed (%d)", err);
        return -lwip_errno(err);
    }
    err = tcp_output(sock->sock_tpcb);
    if (err != ERR_OK) {
        dlog("tcp_output failed (%d)", err);
        return -lwip_errno(err);
    }
    tcp_trace("write-out", index, sock->state, (long)to_write);
    return (ssize_t)to_write;
}

static ssize_t tcp_socket_recv(int index, char *buf, size_t len, int flags) {
    socket_t *sock = socket_lookup(index);
    if (sock == NULL) {
        return -EBADF;
    }
    // printf("DIAG|%lu|recv sock=%d state=%d rx_len=%zu fl=%x\n", diag_ms(), index,
    //        sock->state, sock->rx_len, flags);
    const bool nonblock = (flags & O_NONBLOCK) != 0;

    /* Buffered bytes first, then EOF once the ring is drained, then park.
     * The gate is re-read after every wake, so a peer close or a connection
     * failure while this cothread is parked is seen here rather than on a
     * PCB socket_err_func has already dropped. */
    for (;;) {
        tcp_recv_gate_t gate = tcp_recv_gate(sock->state, sock->rx_len != 0, nonblock);
        if (gate == tcp_recv_copy) {
            break;
        }
        if (gate == tcp_recv_eof) {
            return 0;
        }
        if (gate == tcp_recv_would_block) {
            return -EAGAIN;
        }
        if (gate == tcp_recv_not_connected) {
            return -ENOTCONN;
        }
        // Block until data received, the peer closes, or the socket fails
        microkit_cothread_semaphore_wait(&sock->recv_sem);
    }

    size_t copied = 0;
    while (copied != len) {
        size_t to_copy = MIN(len - copied, tcp_rx_read_span(sock->rx_head, sock->rx_len, SOCKET_BUF_SIZE));
        if (to_copy == 0) {
            break;
        }
        memcpy(buf + copied, sock->rx_buf + sock->rx_head, to_copy);
        sock->rx_head = tcp_rx_advance(sock->rx_head, to_copy, SOCKET_BUF_SIZE);
        sock->rx_len -= to_copy;
        copied += to_copy;
    }

    /* Reopen the receive window. tcp_recved takes a u16_t and the ring holds
     * 2 MB, so a large read has to be reported over several calls: passing
     * the length straight through truncated it modulo 65536, and a read of
     * exactly 64 KiB reported nothing at all and wedged the connection. */
    size_t remaining = copied;
    while (remaining != 0) {
        u16_t chunk = tcp_recved_chunk(remaining);
        tcp_recved(sock->sock_tpcb, chunk);
        remaining -= chunk;
    }
    return (ssize_t)copied;
}

static int tcp_socket_readable(int index) {
    socket_t *socket = socket_lookup(index);
    /* Readiness, not errno: an index that is not a socket is simply never
     * ready, which is what the poll paths expect to read here. */
    if (socket == NULL) {
        return 0;
    }
    // For listening sockets, "readable" means pending connections
    if (socket->state == socket_state_listening) {
        accept_queue_t *q = &socket->accept_queue;
        return !tcp_backlog_empty(q->head, q->tail);
    }

    // For connected sockets, "readable" means data available to read
    return (int)socket->rx_len;
}

/*
 * Whether a poll/select should report the socket ready for writing. The rule
 * is tcp_state_writable in runtime_tcp_state.h, which explains why the state
 * decides this and not the transmit pool alone.
 */
static int tcp_socket_writable(int index) {
    socket_t *sock = socket_lookup(index);
    if (sock == NULL) {
        return 0;
    }

    int writable = tcp_state_writable(sock->state, !net_queue_empty_free(&net_tx_handle));

    tcp_trace("writable", index, sock->state, writable);
    return writable;
}

static int tcp_socket_hup(int index) {
    const socket_t *socket = socket_lookup(index);
    return socket != NULL && tcp_state_hup(socket->state);
}

static int tcp_socket_err(int index) {
    const socket_t *socket = socket_lookup(index);
    if (socket != NULL && socket->state == socket_state_error) {
        /* Positive: this reaches the application through SO_ERROR. */
        return socket->last_error ? (int)socket->last_error : ECONNRESET;
    }
    return 0;
}

/*
 * lwIP hands a new connection over here. Answering anything but ERR_OK means
 * refusing it, and lwIP then aborts newpcb itself, so no failure path below
 * may close or abort it: doing both is a double free. Detaching first is
 * still required wherever this function has already registered callbacks, so
 * lwIP's abort cannot call back into a slot that has been released.
 */
static err_t tcp_socket_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    socket_t *listen_socket = (socket_t *)arg;
    assert(listen_socket != NULL);
    if (listen_socket == NULL || !tcp_accept_allowed(listen_socket->state)) {
        /* The listening socket was closed while this connection was being
         * established. Nothing is attached to newpcb yet. */
        return ERR_ARG;
    }

    if (err != ERR_OK) {
        /* Already an err_t. Negating an errno here produced a value lwIP
         * would read as some unrelated error. */
        return err;
    }

    /* Pre-allocate a socket so we can register the recv/sent/err callbacks on
     * the new PCB immediately.  Without this, data arriving on the connection
     * before the application calls accept() is silently discarded by lwIP
     * (no recv callback = ACK but drop).  gen_tcp:accept blocks behind ERTS's
     * poll loop while nc has already sent data + FIN, so the race is real. */
    int new_index = socket_allocate();
    if (new_index < 0) {
        /* Nothing is attached to newpcb, so lwIP's abort reaches nobody. */
        return ERR_MEM;
    }
    socket_t *socket = &sockets[new_index];
    socket->sock_tpcb = newpcb;
    socket_set_state(socket, socket_state_connected);
    tcp_err(socket->sock_tpcb, socket_err_func);
    tcp_arg(socket->sock_tpcb, socket);
    tcp_sent(newpcb, socket_sent_callback);
    tcp_recv(newpcb, socket_recv_callback);

    /* Every semaphore, not just the two this connection will use: the slot
     * may have been left by a socket that was connecting or listening. */
    microkit_cothread_semaphore_init(&socket->accept_queue.accept_sem);
    microkit_cothread_semaphore_init(&socket->connect_sem);
    microkit_cothread_semaphore_init(&socket->recv_sem);
    microkit_cothread_semaphore_init(&socket->send_sem);

    accept_queue_t *q = &listen_socket->accept_queue;

    if (!tcp_backlog_push(q->pending_socket_indices, MAX_LISTEN_BACKLOG, &q->head, q->tail, new_index)) {
        /* Backlog full: tear down the socket we just allocated. Detach the
         * callbacks first, then leave newpcb to lwIP, which aborts it
         * because this returns an error. */
        socket_detach_pcb(newpcb);
        socket_release(socket);
        // Wake the accept() call to handle the insufficient backlog case
        socket_wake(&listen_socket->accept_queue.accept_sem);
        return ERR_MEM;
    }

    // printf("DIAG|%lu|accept_cb queued sock=%d waiter=%d\n", diag_ms(), new_index,
    //        !microkit_cothread_semaphore_is_queue_empty(
    //            &listen_socket->accept_queue.accept_sem));
    socket_wake(&listen_socket->accept_queue.accept_sem);

    return SOCK_SUCC;
}

static int tcp_socket_listen(int index, int backlog) {
    socket_t *socket = socket_lookup(index);
    if (socket == NULL) {
        return -EBADF;
    }

    if (!tcp_listen_allowed(socket->state)) {
        return -EINVAL;
    }

    // lwIP docs: The tcp_listen() function returns a new connection identifier,
    // and the one passed as an argument to the function will be deallocated.
    struct tcp_pcb *newpcb = tcp_listen_with_backlog(socket->sock_tpcb, tcp_listen_backlog(backlog));
    if (newpcb == NULL) {
        /* Out of listen PCBs. lwIP deallocates the old one only on success,
         * so the socket still owns it and stays closable. The assert this
         * replaces was compiled out, leaving a null dereference below. */
        dlog("couldn't listen on socket %d", index);
        return -ENOMEM;
    }
    socket->sock_tpcb = newpcb;
    socket_set_state(socket, socket_state_listening);
    assert(socket->sock_tpcb->state == LISTEN);

    tcp_accept(socket->sock_tpcb, tcp_socket_accept_cb);

    return SOCK_SUCC;
}

static int tcp_socket_accept(int listen_index, int flags) {
    socket_t *listen_socket = socket_lookup(listen_index);
    if (listen_socket == NULL) {
        return -EBADF;
    }
    if (!tcp_accept_allowed(listen_socket->state)) {
        return -EINVAL;
    }

    accept_queue_t *q = &listen_socket->accept_queue;

    if (tcp_backlog_empty(q->head, q->tail)) {
        if (flags & O_NONBLOCK) {
            // static unsigned long eagain_count;
            // if ((eagain_count++ & 0x3f) == 0) {
            //     printf("DIAG|%lu|accept EAGAIN cnt=%lu\n", diag_ms(), eagain_count);
            // }
            return -EAGAIN;
        }
        // printf("DIAG|%lu|accept BLOCKING fl=%x\n", diag_ms(), flags);
        microkit_cothread_semaphore_wait(&(q->accept_sem));
        // printf("DIAG|%lu|accept sem-woke\n", diag_ms());
    }

    int new_index = 0;
    if (!tcp_backlog_pop(q->pending_socket_indices, MAX_LISTEN_BACKLOG, q->head, &q->tail, &new_index)) {
        return -ENOMEM;
    }

    /* The accept callback already allocated the socket and registered all
     * callbacks (recv/sent/err) on the new PCB before enqueuing this index.
     * We just dequeue, take the caller's reference, and return. */
    if (socket_lookup(new_index) == NULL) {
        dlog("backlog of socket %d held invalid index %d", listen_index, new_index);
        return -EBADF;
    }

    /* The queued socket carries no reference: the backlog held it, and this
     * accept is what hands it to the application. */
    socket_refcount[new_index] = 1;

    // printf("DIAG|%lu|accept -> sock=%d\n", diag_ms(), new_index);
    return new_index;
}

static int tcp_socket_bind(int index, uint32_t addr, uint16_t port) {
    socket_t *sock = socket_lookup(index);
    if (sock == NULL) {
        return -EBADF;
    }

    if (!tcp_bind_allowed(sock->state)) {
        return -EINVAL;
    }

    ip_addr_t ipaddr;
    ip4_addr_set_u32(&ipaddr, addr);

    // Check if addr is available on a local interface (INADDR_ANY always allowed)
    // Assumes waiting for DHCP ready
    if (addr != INADDR_ANY) {
        struct netif *netif;
        bool found = false;
        NETIF_FOREACH(netif) {
            if (ip4_addr_eq(netif_ip4_addr(netif), ip_2_ip4(&ipaddr))) {
                found = true;
                break;
            }
        }
        if (!found) {
            return -EADDRNOTAVAIL;
        }
    }

    err_t err = tcp_bind(sock->sock_tpcb, &ipaddr, port);
    if (err != ERR_OK) {
        return -lwip_errno(err);
    }

    socket_set_state(sock, socket_state_bound);

    return SOCK_SUCC;
}

static int tcp_socket_getsockname(int index, uint32_t *addr, uint16_t *port) {
    socket_t *socket = socket_lookup(index);
    if (socket == NULL) {
        return -EBADF;
    }

    if (socket->state != socket_state_connected && socket->state != socket_state_bound) {
        return -ENOTCONN;
    }

    *addr = ip4_addr_get_u32(&socket->sock_tpcb->local_ip);
    *port = socket->sock_tpcb->local_port;

    return SOCK_SUCC;
}

static int tcp_socket_getpeername(int index, uint32_t *addr, uint16_t *port) {
    socket_t *socket = socket_lookup(index);
    if (socket == NULL) {
        return -EBADF;
    }

    if (socket->state != socket_state_connected) {
        return -ENOTCONN;
    }

    *addr = ip4_addr_get_u32(&socket->sock_tpcb->remote_ip);
    *port = socket->sock_tpcb->remote_port;

    return SOCK_SUCC;
}

libc_socket_config_t socket_config = (libc_socket_config_t) {
    .socket_allocate = socket_allocate,
    .tcp_socket_init = tcp_socket_init,
    .tcp_socket_connect = tcp_socket_connect,
    .tcp_socket_close = tcp_socket_close,
    .tcp_socket_dup = tcp_socket_dup,
    .tcp_socket_write = tcp_socket_write,
    .tcp_socket_recv = tcp_socket_recv,
    .tcp_socket_readable = tcp_socket_readable,
    .tcp_socket_writable = tcp_socket_writable,
    .tcp_socket_hup = tcp_socket_hup,
    .tcp_socket_err = tcp_socket_err,
    .tcp_socket_listen = tcp_socket_listen,
    .tcp_socket_accept = tcp_socket_accept,
    .tcp_socket_bind = tcp_socket_bind,
    .tcp_socket_getsockname = tcp_socket_getsockname,
    .tcp_socket_getpeername = tcp_socket_getpeername,
};
