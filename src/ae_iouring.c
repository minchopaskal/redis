/*
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * io_uring based event processing for Redis - PoC
 *
 * Replaces the epoll-based event loop for client connections with
 * io_uring recv/send/accept. Non-client fds (module pipe, etc.) still
 * use epoll via the standard aeApiPoll path; aeProcessEvents performs
 * a non-blocking epoll poll and delegates the blocking wait to
 * aeIOUringProcessCQEs.
 *
 * Per-client state machine (driven by CQEs):
 *   ACCEPT    -> submit recv SQE
 *   READ CQE  -> process input; if pending replies submit send SQE,
 *                otherwise submit another recv SQE
 *   WRITE CQE -> if more output pending submit send SQE,
 *                otherwise submit recv SQE
 *
 * One ring per thread: the main thread owns the listen socket
 * (multishot accept) and accepts new clients; IO threads receive
 * clients handed to them by the main thread and drive their own
 * recv/send. IO-thread rings share the kernel io-wq with the main
 * ring via IORING_SETUP_ATTACH_WQ.
 */

#ifdef HAVE_IO_URING

#include "server.h"
#include "connhelpers.h"
#include "ae_iouring.h"
#include "liburing.h"

#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>

#define IOURING_SQ_ENTRIES 8192
#define IOURING_CQ_ENTRIES (IOURING_SQ_ENTRIES * 4)
#define IOURING_READBUF_SIZE (16 * 1024)

#define IOURING_REQ_ACCEPT   1
#define IOURING_REQ_READ     2
#define IOURING_REQ_WRITE    3
#define IOURING_REQ_CLOSE    4
#define IOURING_REQ_READ_EVN 5

struct aeIOUringState {
    struct io_uring ring;
    int initialized;
    int listen_fd;
    aeIOUringFdState *fd_states;
    int fd_states_size;
};

/* ------------------------------------------------------------------ */
/* User-data encoding: lower 32 bits = request type, upper 32 = fd    */
/*                                                                    */
/* The kernel echoes sqe->user_data back on the matching CQE, which   */
/* is the only way to recover context for a completion. Packing both  */
/* the request type and the fd avoids a per-SQE allocation.           */
/* ------------------------------------------------------------------ */

static uint64_t iouringUserData(int reqType, int fd) {
    return (uint64_t)reqType | ((uint64_t)(unsigned int)fd << 32);
}

static int iouringGetReqType(uint64_t ud) {
    return (int)(ud & 0xffffffff);
}

static int iouringGetFd(uint64_t ud) {
    return (int)(ud >> 32);
}

/* ------------------------------------------------------------------ */
/* Forward declarations for CQE draining (used by iouringDispatchCQE)      */
/* ------------------------------------------------------------------ */
static void handleAcceptCQE(aeIOUringState *state, aeEventLoop *el,
                            struct io_uring_cqe *cqe);
static void handleReadCQE(aeIOUringState *state, int fd,
                          struct io_uring_cqe *cqe);
static void handleWriteCQE(aeIOUringState *state, int fd,
                           struct io_uring_cqe *cqe);
static void handleCloseCQE(aeIOUringState *state, int fd);
static void handleReadEventNotifierCQE(aeIOUringState *state, aeEventLoop *el,
                                       int fd, struct io_uring_cqe *cqe);


static inline void iouringDispatchCQE(aeIOUringState *state, aeEventLoop *el,
                               struct io_uring_cqe *cqe) {
    int req_type = iouringGetReqType(cqe->user_data);
    int req_fd   = iouringGetFd(cqe->user_data);

    switch (req_type) {
    case IOURING_REQ_ACCEPT:
        handleAcceptCQE(state, el, cqe);
        break;
    case IOURING_REQ_READ:
        handleReadCQE(state, req_fd, cqe);
        break;
    case IOURING_REQ_WRITE:
        handleWriteCQE(state, req_fd, cqe);
        break;
    case IOURING_REQ_CLOSE:
        handleCloseCQE(state, req_fd);
        break;
    case IOURING_REQ_READ_EVN:
        handleReadEventNotifierCQE(state, el, req_fd, cqe);
        break;
    default:
        serverLog(LL_WARNING,
                  "io_uring: unknown req type %d fd=%d",
                  req_type, req_fd);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Drain everything currently in the CQ. Called when the CQ has
 * overflowed (back-pressure under IORING_FEAT_NODROP) or when the SQ
 * is full -- each reaped CQE may free an SQ slot via internal state. */
static int iouringDrainCQEs(aeIOUringState *state, aeEventLoop *el) {
    unsigned head;
    struct io_uring_cqe *cqe;
    int count = 0;

    io_uring_for_each_cqe(&state->ring, head, cqe) {
        iouringDispatchCQE(state, el, cqe);
        count++;
    }

    io_uring_cq_advance(&state->ring, count);
    return count;
}

/* Get an SQE, guaranteed to succeed.
 *
 * Fast path: io_uring_get_sqe returns immediately.
 *
 * Slow path: SQ is full. Submit pending SQEs so the kernel can drain
 * them, retry, and only as a last resort yield. The ring has a finite
 * size so progress is bounded. */
static struct io_uring_sqe *iouringGetSqe(aeIOUringState *state) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (likely(sqe != NULL)) return sqe;

    while (1) {
        sqe = io_uring_get_sqe(&state->ring);
        if (sqe != NULL) return sqe;

        io_uring_submit(&state->ring);

        sqe = io_uring_get_sqe(&state->ring);
        if (sqe != NULL) return sqe;

        usleep(50);
    }
}

/* With IORING_FEAT_NODROP the kernel back-pressures rather than
 * silently dropping CQEs, but we still want to drain proactively
 * before queuing more work: an overflowed CQ stalls submission until
 * room is made. */
static void iouringCheckCQOverflow(aeIOUringState *state, aeEventLoop *el) {
    if (io_uring_cq_has_overflow(&state->ring)) {
        serverLog(LL_VERBOSE,
                  "io_uring: CQ overflow detected, draining CQEs");
        iouringDrainCQEs(state, el);
    }
}

/* Per-fd state is kept in an array indexed by fd. Grows on demand
 * (doubling, but at least up to `fd + 1`) so new high-numbered client
 * fds don't have to reallocate every time. New entries are zeroed so
 * callers can treat them as fresh state. */
static aeIOUringFdState *iouringGetFdState(aeIOUringState *state, int fd) {
    if (fd >= state->fd_states_size) {
        int new_size = state->fd_states_size * 2;
        if (new_size <= fd) new_size = fd + 1;
        state->fd_states = zrealloc(state->fd_states,
                                    sizeof(aeIOUringFdState) * new_size);
        memset(state->fd_states + state->fd_states_size, 0,
               sizeof(aeIOUringFdState) * (new_size - state->fd_states_size));
        state->fd_states_size = new_size;
    }
    return &state->fd_states[fd];
}

/* ------------------------------------------------------------------ */
/* SQE submission helpers                                             */
/* ------------------------------------------------------------------ */

static bool iothreadCheckCron(client *c) {
    return c->io_flags & CLIENT_IO_PENDING_CRON;
}

/* IOSQE_ASYNC forces the recv through an io-wq worker rather than
 * relying on the inline / FAST_POLL fast path. This trades a small
 * per-op cost for fairness across many concurrent clients: a slow
 * client cannot delay the issuing task's wait loop. */
static void iouringSubmitRecv(aeIOUringState *state, int fd) {
    aeIOUringFdState *fs = iouringGetFdState(state, fd);
    if (!fs->readbuf) {
        fs->readbuf = zmalloc(IOURING_READBUF_SIZE);
    }
    connection *conn = fs->conn;
    client *c = connGetPrivateData(conn);

    /* When we have a pending cron job we don't submit the next SQE in order
     * to give a chance to the IOThreadClientCron to send the client back to
     * main thread for running the cron job on it. */
    if (iothreadCheckCron(c)) {
        return;
    }

    struct io_uring_sqe *sqe = iouringGetSqe(state);
    io_uring_prep_recv(sqe, fd, fs->readbuf, IOURING_READBUF_SIZE, 0);
    sqe->user_data = iouringUserData(IOURING_REQ_READ, fd);
    io_uring_sqe_set_flags(sqe, IOSQE_ASYNC);
    c->iouring_flags |= CLIENT_IO_WAIT_RECV_CQE;
}

/* `buf` must remain valid until the matching CQE arrives. Callers
 * pass pointers into the client's reply buffers, which are owned by
 * the client and not freed until the write completes (see
 * handleWriteCQE). */
static void iouringSubmitSend(aeIOUringState *state, int fd,
                              const void *buf, int len) {
    aeIOUringFdState *fs = iouringGetFdState(state, fd);
    connection *conn = fs->conn;
    client *c = connGetPrivateData(conn);

    if (iothreadCheckCron(c)) {
        return;
    }

    struct io_uring_sqe *sqe = iouringGetSqe(state);
    io_uring_prep_send(sqe, fd, buf, len, 0);
    sqe->user_data = iouringUserData(IOURING_REQ_WRITE, fd);
    io_uring_sqe_set_flags(sqe, IOSQE_ASYNC);
    c->iouring_flags |= CLIENT_IO_WAIT_WRITE_CQE;
}

/* Submit the next outgoing chunk for the client, or fall back to a
 * recv if nothing is pending. The static buffer (c->buf) is checked
 * before the reply list because partially-sent static data must be
 * flushed before queued reply blocks are touched.
 *
 * CLIENT_IO_WAIT_CQE marks the client as having an in-flight send;
 * the IO-thread main loop uses it to avoid double-submitting. */
static void iouringSubmitClientWrite(aeIOUringState *state, client *c) {
    int fd = c->conn->fd;

    if (c->bufpos > c->sentlen) {
        iouringSubmitSend(state, fd,
                          c->buf + c->sentlen,
                          c->bufpos - c->sentlen);
        return;
    }

    if (listLength(c->reply) > 0) {
        clientReplyBlock *block = listNodeValue(listFirst(c->reply));
        int offset = c->sentlen;
        iouringSubmitSend(state, fd,
                          block->buf + offset,
                          block->used - offset);
        return;
    }

    iouringSubmitRecv(state, fd);
}

/* Arms a one-shot read on an eventfd / pipe used to wake this thread
 * (see iothread.c notifiers). prep_read -- not prep_recv -- because
 * eventfd and pipes are not sockets. No IOSQE_ASYNC: notifier fds are
 * cheap and we want the FAST_POLL fast path so the wakeup latency is
 * minimal. The kernel writes the 8-byte counter into fs->evn_buf
 * which lives across the in-flight period. */
static void iouringSubmitReadEventNotifier(aeIOUringState *state, int fd) {
    aeIOUringFdState *fs = iouringGetFdState(state, fd);
    struct io_uring_sqe *sqe = iouringGetSqe(state);
    io_uring_prep_read(sqe, fd, &fs->evn_buf, sizeof(fs->evn_buf), 0);
    sqe->user_data = iouringUserData(IOURING_REQ_READ_EVN, fd);
}

/* ------------------------------------------------------------------ */
/* Init / Cleanup                                                     */
/* ------------------------------------------------------------------ */

/* Initialize io_uring for `eventLoop`.
 *
 *   listen_fd >= 0 : main-thread ring; takes ownership of the listen
 *                    socket and arms multishot accept on it.
 *   listen_fd <  0 : IO-thread ring; no accept, handles only the
 *                    clients the main thread hands to it.
 *
 * SINGLE_ISSUER + DEFER_TASKRUN are the recommended modern setup:
 * only the creating task may submit (cheap, no locking) and kernel
 * task work is deferred until that task waits, which keeps latency
 * predictable and avoids interrupts on hot paths. */
int aeIOUringInit(aeEventLoop *eventLoop, int listen_fd, int max_bgworkers) {
    aeIOUringState *state = zcalloc(sizeof(*state));

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SINGLE_ISSUER |
                   IORING_SETUP_CQSIZE |
                   IORING_SETUP_DEFER_TASKRUN;
    params.cq_entries = IOURING_CQ_ENTRIES;

    int ret = io_uring_queue_init_params(IOURING_SQ_ENTRIES,
                                         &state->ring, &params);
    if (ret < 0) {
        serverLog(LL_WARNING, "io_uring init failed: %s", strerror(-ret));
        zfree(state);
        return C_ERR;
    }

    /* NODROP (5.5+) makes the kernel back-pressure instead of silently
     * dropping CQEs; without it we would lose completions, leak fds,
     * and leave clients stuck. FAST_POLL (5.7+) lets recv/send avoid
     * the io-wq worker when the fd is not yet ready, which is what
     * makes async socket I/O cheap. */
    uint32_t required = IORING_FEAT_NODROP | IORING_FEAT_FAST_POLL;
    if ((params.features & required) != required) {
        serverLog(LL_WARNING,
                  "io_uring: kernel features 0x%x missing required 0x%x "
                  "(need NODROP + FAST_POLL), falling back to epoll.",
                  params.features, required);
        io_uring_queue_exit(&state->ring);
        zfree(state);
        return C_ERR;
    }

    /* Bound bg-worker count. Without a cap io_uring will spawn a
     * worker per blocked op, which on large boxes (and with many
     * IOSQE_ASYNC submissions) explodes thread counts. */
    long n = max_bgworkers > 0 ? max_bgworkers : sysconf(_SC_NPROCESSORS_ONLN);
    n = n > 0 ? n : 8;
    int max_workers[2] = {n, n};
    io_uring_register(state->ring.ring_fd, IORING_REGISTER_IOWQ_MAX_WORKERS, max_workers, 2);

    state->initialized = 1;
    state->listen_fd   = listen_fd;
    state->fd_states_size = eventLoop->setsize;
    state->fd_states = zcalloc(sizeof(aeIOUringFdState) * state->fd_states_size);

    eventLoop->iouring_state = state;
    eventLoop->iouring_process_cqes = aeIOUringProcessCQEs;
    eventLoop->iouring_cleanup      = aeIOUringCleanup;

    /* Main-thread ring owns the listen socket: remove it from epoll
     * (the standard ae path would otherwise also accept on it) and
     * arm a single multishot accept SQE -- the kernel will keep
     * producing one CQE per accepted connection until it tells us to
     * re-arm (see handleAcceptCQE). */
    if (listen_fd >= 0) {
        aeDeleteFileEvent(eventLoop, listen_fd, AE_READABLE);

        /* Ring is brand new so get_sqe cannot fail. */
        struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
        serverAssert(sqe != NULL);
        io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL, 0);
        sqe->user_data = iouringUserData(IOURING_REQ_ACCEPT, listen_fd);
        io_uring_submit(&state->ring);
    }

    serverLog(LL_NOTICE,
              "io_uring initialized: SQ=%d CQ=%d SQPOLL=%d, "
              "features=0x%x, listen fd=%d",
              IOURING_SQ_ENTRIES, IOURING_CQ_ENTRIES,
              params.flags & IORING_SETUP_SQPOLL,
              params.features, listen_fd);
    return C_OK;
}

void aeIOUringCleanup(aeEventLoop *eventLoop) {
    aeIOUringState *state = eventLoop->iouring_state;
    if (!state) return;

    for (int i = 0; i < state->fd_states_size; i++) {
        if (state->fd_states[i].readbuf)
            zfree(state->fd_states[i].readbuf);
    }
    zfree(state->fd_states);
    io_uring_queue_exit(&state->ring);
    zfree(state);
    eventLoop->iouring_state = NULL;
}

void aeIOUringDeactivateFd(aeEventLoop *eventLoop, int fd) {
    aeIOUringState *state = eventLoop->iouring_state;
    if (!state || fd < 0 || fd >= state->fd_states_size) return;

    aeIOUringFdState *fs = &state->fd_states[fd];
    fs->active   = 0;
    fs->conn     = NULL;
    fs->read_len = 0;
    fs->read_pos = 0;
}

aeIOUringFdState *aeIOUringGetFdState(aeEventLoop *el, int fd) {
    aeIOUringState *state = el ? el->iouring_state : NULL;
    if (!state || fd < 0 || fd >= state->fd_states_size) return NULL;
    aeIOUringFdState *fs = &state->fd_states[fd];
    if (!fs->active) return NULL;
    return fs;
}

/* ------------------------------------------------------------------ */
/* CQE Processing                                                     */
/* ------------------------------------------------------------------ */

static inline void iouringArmMultishotAccept(aeIOUringState *state) {
    struct io_uring_sqe *sqe = iouringGetSqe(state);
    io_uring_prep_multishot_accept(sqe, state->listen_fd, NULL, NULL, 0);
    sqe->user_data = iouringUserData(IOURING_REQ_ACCEPT, state->listen_fd);
}

static void handleAcceptCQE(aeIOUringState *state, aeEventLoop *el,
                            struct io_uring_cqe *cqe) {
    if (cqe->res < 0) {
        serverLog(LL_WARNING, "io_uring accept error: %s",
                  strerror(-cqe->res));
        iouringArmMultishotAccept(state);
        return;
    }

    int client_fd = cqe->res;
    serverLog(LL_VERBOSE, "io_uring: accepted fd=%d", client_fd);

    /* Create the connection with the TCP type first so that
     * acceptCommonHandler's connSetReadHandler / connSetWriteHandler
     * calls go through normal sockets code. We then swap the
     * connection type to the io_uring type so that subsequent
     * connRead/connWrite from this thread route through our ring. */
    connection *conn = connCreateAccepted(el, connectionTypeTcp(),
                                          client_fd, NULL);

    acceptCommonHandler(conn, 0, NULL);
    /* Remove any epoll registration that acceptCommonHandler may have
     * installed -- this fd is owned by the ring now. */
    aeDeleteFileEvent(el, client_fd, AE_READABLE);
    conn->type = connectionTypeIOUring();

    /* acceptCommonHandler may have:
     *  - closed the connection (rejected / over maxclients / ...)
     *  - left the client on this (main) event loop, or
     *  - handed it to an IO thread (assignClientToIOThread), which
     *    rebinds conn->el to the IO thread's loop. In that case the
     *    IO thread will own the initial recv via aeIOUringClientSetup.
     * Only submit the initial recv when the client stayed here. */
    if (conn->fd > 0 && conn->el == el) {
        aeIOUringFdState *fs = iouringGetFdState(state, client_fd);
        fs->active   = 1;
        fs->conn     = conn;
        fs->read_len = 0;
        fs->read_pos = 0;
        iouringSubmitRecv(state, client_fd);

        /* Multishot accept normally keeps producing CQEs without
         * being re-armed (F_MORE set). On the rare CQE without F_MORE
         * the kernel is asking us to re-arm. */
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
            iouringArmMultishotAccept(state);
        }
    }
}

static void handleReadCQE(aeIOUringState *state, int fd,
                          struct io_uring_cqe *cqe) {
    aeIOUringFdState *fs = iouringGetFdState(state, fd);

    if (!fs->active || !fs->conn) return;
    connection *conn = fs->conn;
    client *c = connGetPrivateData(conn);
    if (!c) {
        fs->active = 0;
        return;
    }

    c->iouring_flags &= ~CLIENT_IO_WAIT_RECV_CQE;

    /* res == 0 is EOF (peer closed); res < 0 is an errno. Both
     * terminate the connection. */
    if (cqe->res <= 0) {
        if (cqe->res < 0) {
            conn->last_errno = -cqe->res;
            serverLog(LL_VERBOSE, "io_uring read error fd=%d: %s",
                      fd, strerror(-cqe->res));
        }
        conn->state = (cqe->res == 0) ? CONN_STATE_CLOSED : CONN_STATE_ERROR;
        freeClientAsync(c);
        fs->active = 0;
        return;
    }

    /* Hand the just-received bytes off to readQueryFromClient via the
     * connection's read method. connIOUringRead (in socket.c) memcpys
     * out of fs->readbuf using read_pos/read_len, so no extra syscall
     * is needed inside the handler. */
    fs->read_len = cqe->res;
    fs->read_pos = 0;

    if (conn->read_handler) {
        connIncrRefs(conn);
        conn->read_handler(conn);
        connDecrRefs(conn);
        /* The handler may have requested close; finish that here
         * once we drop the last reference. */
        if (conn->flags & CONN_FLAG_CLOSE_SCHEDULED) {
            if (!connHasRefs(conn)) connClose(conn);
            fs->active = 0;
            return;
        }
    }

    fs->read_len = 0;
    fs->read_pos = 0;

    /* The handler may have freed the client. */
    if (!fs->active || !fs->conn) return;
    c = connGetPrivateData(conn);
    if (!c) {
        fs->active = 0;
        return;
    }

    /* If readQueryFromClient parsed a full command and enqueued the
     * client to the main thread for execution, the main thread now
     * owns it. Do not submit another SQE here; the next SQE will be
     * issued when the main thread bounces the client back via
     * aeIOUringClientSetup / aeIOUringClientStartWrite. */
    if (c->io_flags & CLIENT_IO_PENDING_COMMAND)
        return;

    if (clientHasPendingReplies(c)) {
        iouringSubmitClientWrite(state, c);
    } else {
        iouringSubmitRecv(state, fd);
    }
}

static void handleWriteCQE(aeIOUringState *state, int fd,
                           struct io_uring_cqe *cqe) {
    aeIOUringFdState *fs = iouringGetFdState(state, fd);

    if (!fs->active || !fs->conn) return;

    connection *conn = fs->conn;
    client *c = connGetPrivateData(conn);
    if (!c) {
        fs->active = 0;
        return;
    }
    
    c->iouring_flags &= ~CLIENT_IO_WAIT_WRITE_CQE;

    if (cqe->res < 0) {
        serverLog(LL_VERBOSE, "io_uring write error fd=%d: %s",
                  fd, strerror(-cqe->res));
        freeClientAsync(c);
        fs->active = 0;
        return;
    }

    int nwritten = cqe->res;

    /* Mirrors the bookkeeping in _writevToClient / _writeToClientNonSlave:
     * we were sending either from the static c->buf or from the head
     * block of c->reply, and need to advance sentlen / pop a block
     * once it's fully drained. */
    if (c->bufpos > 0) {
        c->sentlen += nwritten;
        if (c->sentlen >= c->bufpos) {
            c->bufpos  = 0;
            c->sentlen = 0;
        }
    } else if (listLength(c->reply) > 0) {
        c->sentlen += nwritten;
        clientReplyBlock *block = listNodeValue(listFirst(c->reply));
        if ((size_t)c->sentlen >= block->used) {
            c->reply_bytes -= block->size;
            listDelNode(c->reply, listFirst(c->reply));
            c->sentlen = 0;
        }
    }

    c->net_output_bytes += nwritten;
    atomicIncr(server.stat_net_output_bytes, nwritten);

    if (!(c->flags & CLIENT_MASTER))
        c->lastinteraction = server.unixtime;

    if (clientHasPendingReplies(c)) {
        iouringSubmitClientWrite(state, c);
    } else {
        if (c->flags & CLIENT_CLOSE_AFTER_REPLY) {
            freeClientAsync(c);
            fs->active = 0;
            return;
        }
        iouringSubmitRecv(state, fd);
    }
}

static void handleCloseCQE(aeIOUringState *state, int fd) {
    if (fd < 0 || fd >= state->fd_states_size) return;
    aeIOUringFdState *fs = &state->fd_states[fd];
    fs->active = 0;
    fs->conn   = NULL;
}

/* One-shot read of an event-notifier fd (eventfd or pipe). The
 * notifier is what other threads use to wake this thread; we must
 * re-arm after every successful read or the thread goes deaf. */
static void handleReadEventNotifierCQE(aeIOUringState *state, aeEventLoop *el,
                                       int fd, struct io_uring_cqe *cqe) {
    aeIOUringFdState *fs = iouringGetFdState(state, fd);

    if (!fs->active) return;

    if (cqe->res <= 0) {
        if (cqe->res < 0) {
            serverLog(LL_VERBOSE, "io_uring read error fd=%d: %s",
                      fd, strerror(-cqe->res));
        } else {
            /* res == 0 on an eventfd should not happen in normal
             * operation; re-arm and keep going. */
            iouringSubmitReadEventNotifier(state, fd);
        }

        fs->active = 0;
        return;
    }

    fs->evn_proc(el, fd, fs->evn_data, 0);

    iouringSubmitReadEventNotifier(state, fd);
}

/* Drain io_uring completions. Called from aeProcessEvents.
 *
 * timeout_us:
 *    > 0 : block up to this many microseconds for at least one CQE.
 *   == 0 : peek only, do not block.
 *    < 0 : block indefinitely until a CQE arrives. */
int aeIOUringProcessCQEs(aeEventLoop *eventLoop, int64_t timeout_us) {
    aeIOUringState *state = eventLoop->iouring_state;
    if (!state || !state->initialized) return 0;

    iouringCheckCQOverflow(state, eventLoop);

    /* Push any SQEs accumulated since the last iteration to the
     * kernel before we start waiting. */
    io_uring_submit(&state->ring);

    struct __kernel_timespec ts;
    struct __kernel_timespec *tsp = NULL;
    if (timeout_us >= 0) {
        ts.tv_sec  = timeout_us / 1000000LL;
        ts.tv_nsec = (timeout_us % 1000000LL) * 1000LL;
        tsp = &ts;
    }

    /* wait_nr must be 1 when blocking with tsp == NULL (timeout < 0),
     * otherwise we could sleep forever waiting for a batch that
     * never fills. When timeout_us == 0 we want a non-blocking peek.
     * io_uring_for_each_cqe below drains everything in the CQ
     * regardless of wait_nr. */
    unsigned wait_nr;
    if (timeout_us > 0)        wait_nr = 1;
    else if (timeout_us == 0)  wait_nr = 0;
    else                       wait_nr = 1;

    struct io_uring_cqe *cqe = NULL;
    int ret = io_uring_wait_cqes(&state->ring, &cqe, wait_nr, tsp, NULL);
    /* -ETIME: timeout expired, -EINTR: signal, -EAGAIN: peek with
     * empty CQ (wait_nr=0). All expected and harmless. */
    if (ret < 0 && ret != -ETIME && ret != -EINTR && ret != -EAGAIN) {
        serverLog(LL_WARNING, "io_uring_wait_cqes: %s", strerror(-ret));
    }

    unsigned head;
    int cqe_count = 0;

    io_uring_for_each_cqe(&state->ring, head, cqe) {
        iouringDispatchCQE(state, eventLoop, cqe);
        cqe_count++;
    }

    io_uring_cq_advance(&state->ring, cqe_count);
    return cqe_count;
}

/* ------------------------------------------------------------------ */
/* Public helpers for iothread.c                                      */
/* ------------------------------------------------------------------ */

/* (Re-)bind the per-fd state on `state` to client `c`. Idempotent so
 * it can be called on both first bind and when the client returns
 * from the main thread to an IO thread. */
static aeIOUringFdState *iouringEnsureFdState(aeIOUringState *state,
                                              client *c) {
    int fd = c->conn->fd;
    aeIOUringFdState *fs = iouringGetFdState(state, fd);
    fs->active   = 1;
    fs->conn     = c->conn;
    fs->read_len = 0;
    fs->read_pos = 0;
    return fs;
}

/* Called from the IO thread (processClientsFromMainThread) after
 * the connection has been rebound to the IO thread's event loop.
 * Kicks off the read loop for this client on this ring. */
void aeIOUringClientSetup(aeEventLoop *el, client *c) {
    aeIOUringState *state = el->iouring_state;
    if (!state || !state->initialized) return;
    if (!c->conn || c->conn->fd < 0) return;

    iouringEnsureFdState(state, c);
    iouringSubmitRecv(state, c->conn->fd);
}

/* Counterpart to aeIOUringClientSetup for clients that come back from
 * the main thread with pending replies to flush. */
void aeIOUringClientStartWrite(aeEventLoop *el, client *c) {
    aeIOUringState *state = el->iouring_state;
    if (!state || !state->initialized) return;
    if (!c->conn || c->conn->fd < 0) return;

    iouringEnsureFdState(state, c);
    iouringSubmitClientWrite(state, c);
}

/* Register a callback to fire whenever `fd` (an eventfd / pipe used
 * as a wakeup notifier) becomes readable. Used by iothread.c to make
 * its inter-thread notifiers drive io_uring's wait loop instead of
 * sitting in epoll. */
int aeIOUringSetupReadEventNotifier(aeEventLoop *el, int fd,
                                    aeIOUringEventNotifierProc proc, void *clientData) {
    aeIOUringState *state = el ? el->iouring_state : NULL;
    if (!state || !state->initialized) return C_ERR;
    if (fd < 0 || !proc) return C_ERR;
    aeIOUringFdState *fs = iouringGetFdState(state, fd);
    fs->evn_proc = proc;
    fs->evn_data = clientData;
    /* `active` is reused here as the "notifier is armed" flag --
     * handleReadEventNotifierCQE drops it on fatal errors. */
    fs->active = 1;

    iouringSubmitReadEventNotifier(state, fd);

    return C_OK;
}

#endif /* HAVE_IO_URING */

/* ISO C forbids an empty translation unit; with HAVE_IO_URING disabled
 * this file contains no other declarations and would trigger
 * -Wpedantic. A file-scope typedef is a no-op at link time. */
typedef int ae_iouring_translation_unit_placeholder;
