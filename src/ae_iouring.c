/*
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * io_uring based event processing for Redis - PoC
 *
 * This replaces the epoll-based event loop for client connections with
 * io_uring recv/send operations. Non-client fds (module pipe, etc.) still
 * use epoll via the standard aeApiPoll path.
 *
 * State machine per client fd:
 *   ACCEPT -> submit recv SQE
 *   READ CQE  -> process input -> if pending replies: submit send SQE
 *                                  else: submit recv SQE
 *   WRITE CQE -> if more to write: submit send SQE
 *                 else: submit recv SQE
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

struct aeIOUringState {
    struct io_uring ring;
    int initialized;
    int listen_fd;
    aeIOUringFdState *fd_states;
    int fd_states_size;
};

/* ------------------------------------------------------------------ */
/* User-data encoding: lower 32 bits = request type, upper 32 = fd   */
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
/* Forward declarations for CQE draining (used by iouringGetSqe)     */
/* ------------------------------------------------------------------ */
static void handleAcceptCQE(aeIOUringState *state, aeEventLoop *el,
                            struct io_uring_cqe *cqe);
static void handleReadCQE(aeIOUringState *state, aeEventLoop *el,
                           int fd, struct io_uring_cqe *cqe);
static void handleWriteCQE(aeIOUringState *state, aeEventLoop *el,
                            int fd, struct io_uring_cqe *cqe);
static void handleCloseCQE(aeIOUringState *state, int fd);
static void handlePollCQE(aeIOUringState *state, aeEventLoop *el,
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
        handleReadCQE(state, el, req_fd, cqe);
        break;
    case IOURING_REQ_WRITE:
        handleWriteCQE(state, el, req_fd, cqe);
        break;
    case IOURING_REQ_CLOSE:
        handleCloseCQE(state, req_fd);
        break;
    case IOURING_REQ_POLL_FD:
        handlePollCQE(state, el, req_fd, cqe);
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

/*
 * Drain all pending CQEs.  This is called when the SQ is full or the
 * CQ is overflowing – processing completions frees SQ slots.
 * Returns the number of CQEs processed.
 */
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

/*
 * Get an SQE, guaranteed to succeed.
 *
 * Fast path: io_uring_get_sqe usually returns immediately.
 *
 * If the SQ is full we help it along:
 *   1. Submit pending SQEs so the SQPOLL thread picks them up.
 *   2. Drain completed CQEs – each reaped CQE frees an SQ slot.
 *   3. If nothing was drained (everything still in-flight), yield
 *      briefly so the SQPOLL thread makes progress.
 *   4. Repeat.  The ring has a finite number of entries so this
 *      always terminates.
 */
static struct io_uring_sqe *iouringGetSqe(aeIOUringState *state) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (likely(sqe != NULL)) return sqe;

    while (1) {
        io_uring_submit(&state->ring);

        sqe = io_uring_get_sqe(&state->ring);
        if (sqe != NULL) return sqe;

        io_uring_submit(&state->ring);

        sqe = io_uring_get_sqe(&state->ring);
        if (sqe != NULL) return sqe;

        usleep(10);
    }
}

/*
 * Check CQ overflow before submitting new SQEs.
 * If the CQ has overflowed we drain everything first so the kernel
 * can store new completions.  With IORING_FEAT_NODROP the kernel
 * back-pressures instead of dropping, but draining still helps keep
 * latency down.
 */
static void iouringCheckCQOverflow(aeIOUringState *state, aeEventLoop *el) {
    if (io_uring_cq_has_overflow(&state->ring)) {
        serverLog(LL_VERBOSE,
                  "io_uring: CQ overflow detected, draining CQEs");
        iouringDrainCQEs(state, el);
    }
}

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

static void iouringSubmitRecv(aeIOUringState *state, aeEventLoop *el, int fd) {
    aeIOUringFdState *fs = iouringGetFdState(state, fd);
    if (!fs->readbuf) {
        fs->readbuf = zmalloc(IOURING_READBUF_SIZE);
    }

    iouringCheckCQOverflow(state, el);
    struct io_uring_sqe *sqe = iouringGetSqe(state);
    io_uring_prep_recv(sqe, fd, fs->readbuf, IOURING_READBUF_SIZE, 0);
    sqe->user_data = iouringUserData(IOURING_REQ_READ, fd);
    io_uring_sqe_set_flags(sqe, IOSQE_ASYNC);
}

static void iouringSubmitSend(aeIOUringState *state, aeEventLoop *el,
                              int fd, const void *buf, int len) {
    iouringCheckCQOverflow(state, el);
    struct io_uring_sqe *sqe = iouringGetSqe(state);
    io_uring_prep_send(sqe, fd, buf, len, 0);
    sqe->user_data = iouringUserData(IOURING_REQ_WRITE, fd);
    io_uring_sqe_set_flags(sqe, IOSQE_ASYNC);
}

/*
 * Look at the client's reply buffers and submit a send SQE for the next
 * chunk that needs to be written.  If nothing is pending, submit a recv
 * SQE instead (back to reading state).
 */
static void iouringSubmitClientWrite(aeIOUringState *state, aeEventLoop *el,
                                     client *c) {
    int fd = c->conn->fd;

    if (c->bufpos > c->sentlen) {
        iouringSubmitSend(state, el, fd,
                          c->buf + c->sentlen,
                          c->bufpos - c->sentlen);
        c->io_flags |= CLIENT_IO_WAIT_CQE;
        return;
    }

    if (listLength(c->reply) > 0) {
        clientReplyBlock *block = listNodeValue(listFirst(c->reply));
        int offset = c->sentlen;
        iouringSubmitSend(state, el, fd,
                          block->buf + offset,
                          block->used - offset);
        c->io_flags |= CLIENT_IO_WAIT_CQE;
        return;
    }

    /* Nothing left to write -> back to reading */
    iouringSubmitRecv(state, el, fd);
}

/* ------------------------------------------------------------------ */
/* Init / Cleanup                                                     */
/* ------------------------------------------------------------------ */

int aeIOUringInit(aeEventLoop *eventLoop, int listen_fd) {
    aeIOUringState *state = zcalloc(sizeof(*state));

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL |
                   IORING_SETUP_SINGLE_ISSUER |
                   IORING_SETUP_CQSIZE;
    params.cq_entries = IOURING_CQ_ENTRIES;

    int ret = io_uring_queue_init_params(IOURING_SQ_ENTRIES,
                                         &state->ring, &params);
    if (ret < 0) {
        serverLog(LL_WARNING, "io_uring init failed: %s", strerror(-ret));
        zfree(state);
        return C_ERR;
    }

    /*
     * Required kernel features – bail out if missing:
     *
     * IORING_FEAT_NODROP (5.5+): the kernel back-pressures when the CQ
     *   is full instead of silently dropping CQEs.  Without it we'd
     *   lose completions, leak fds, and leave clients stuck.
     *
     * IORING_FEAT_FAST_POLL (5.7+): the kernel does internal polling
     *   for socket ops (recv/send).  Without it those SQEs would need
     *   data to be ready at submission time, making async recv/send
     *   useless.
     */
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

    long n = sysconf(_SC_NPROCESSORS_ONLN);
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

    /* Main-thread ring owns the listen socket: remove it from epoll and
     * install multishot accept.  IO-thread rings (listen_fd < 0) skip
     * this – they only handle clients that the main thread hands to
     * them. */
    if (listen_fd >= 0) {
        aeDeleteFileEvent(eventLoop, listen_fd, AE_READABLE);

        /* Ring is empty so get_sqe cannot fail. */
        struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
        serverAssert(sqe != NULL);
        io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL, 0);
        sqe->user_data = iouringUserData(IOURING_REQ_ACCEPT, listen_fd);
        io_uring_submit(&state->ring);
    }

    serverLog(LL_NOTICE,
              "io_uring initialized: SQ=%d CQ=%d SQPOLL mode, "
              "features=0x%x, listen fd=%d",
              IOURING_SQ_ENTRIES, IOURING_CQ_ENTRIES,
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

    /*
     * Create the connection using CT_IOUring right away, so any
     * connSetReadHandler/connSetWriteHandler calls made by createClient
     * and clientAcceptHandler are no-ops (no epoll events created).
     */
    connection *conn = connCreateAccepted(el, connectionTypeTcp(),
                                          client_fd, NULL);

    acceptCommonHandler(conn, 0, NULL);
    aeDeleteFileEvent(el, client_fd, AE_READABLE);
    conn->type = connectionTypeIOUring();
    /*
     * acceptCommonHandler may:
     *   - close the connection (rejected / over maxclients / etc.)
     *   - leave the client bound to this (main) event loop
     *   - call assignClientToIOThread(), which unbinds conn from this
     *     event loop (conn->el = NULL) and queues the client for an
     *     IO thread.  In that case the IO thread will submit the
     *     initial recv SQE in processClientsFromMainThread.
     */

    if (conn->fd > 0 && conn->el == el) {
        /* Client stayed on the main thread – own its initial recv. */
        aeIOUringFdState *fs = iouringGetFdState(state, client_fd);
        fs->active   = 1;
        fs->conn     = conn;
        fs->read_len = 0;
        fs->read_pos = 0;
        iouringSubmitRecv(state, el, client_fd);

        /* Re-arm multishot accept if the kernel disabled it. */
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
            iouringArmMultishotAccept(state);
        }
    }
}

static void handleReadCQE(aeIOUringState *state, aeEventLoop *el,
                           int fd, struct io_uring_cqe *cqe) {
    // serverLog(LL_NOTICE, "iouring: read cqe 1 (submit send)");
    aeIOUringFdState *fs = iouringGetFdState(state, fd);

    if (!fs->active || !fs->conn) return;
    connection *conn = fs->conn;
    client *c = connGetPrivateData(conn);
    // serverLog(LL_NOTICE, "iouring: read cqe 2 fd=%d (submit send)", c->conn->fd);
    if (!c) {
        fs->active = 0;
        return;
    }

    if (cqe->res <= 0) {
        if (cqe->res < 0) {
            conn->last_errno = -cqe->res;
            serverLog(LL_VERBOSE, "io_uring read error fd=%d: %s",
                      fd, strerror(-cqe->res));
        }
        /*
         * Mark the connection state so connRead inside readQueryFromClient
         * will see the right thing if it's ever called again.
         */
        conn->state = (cqe->res == 0) ? CONN_STATE_CLOSED : CONN_STATE_ERROR;
        freeClientAsync(c);
        fs->active = 0;
        return;
    }

    /*
     * Store the received data in the per-fd state.  The connection's
     * read method (connIOUringRead in socket.c) will memcpy from here
     * when readQueryFromClient calls connRead().
     */
    fs->read_len = cqe->res;
    fs->read_pos = 0;

    /*
     * Call the real read handler.  readQueryFromClient will call
     * connRead() which dispatches through the connection type's .read
     * method – for io_uring connections that returns data from
     * fs->readbuf without a syscall.
     */
    if (conn->read_handler) {
        connIncrRefs(conn);
        conn->read_handler(conn);
        connDecrRefs(conn);
        if (conn->flags & CONN_FLAG_CLOSE_SCHEDULED) {
            if (!connHasRefs(conn)) connClose(conn);
            fs->active = 0;
            return;
        }
    }

    /* Clear residual buffer state after the handler consumed what it needed. */
    fs->read_len = 0;
    fs->read_pos = 0;

    /* The client may have been freed inside the handler. */
    if (!fs->active || !fs->conn) return;
    c = connGetPrivateData(conn);
    if (!c) {
        fs->active = 0;
        return;
    }

    /*
     * If readQueryFromClient + processInputBuffer queued this client
     * to the main thread (a full command is ready to execute), the
     * IO thread must NOT submit another SQE for this fd.  The main
     * thread now owns the client; it will bounce it back to this
     * (or another) IO thread via processClientsFromMainThread, which
     * is where the next SQE will be submitted.
     *
     * Detection: enqueuePendingClientsToMainThread clears both IO
     * read and write enabled flags.  If either is cleared, the
     * client is mid-handoff – leave it alone.
     */
    if (c->io_flags & CLIENT_IO_PENDING_COMMAND)
        return;

    /* ---- decide next action: WRITE or READ ---- */
    if (clientHasPendingReplies(c)) {
        iouringSubmitClientWrite(state, el, c);
    } else {
        iouringSubmitRecv(state, el, fd);
    }
}

static void handleWriteCQE(aeIOUringState *state, aeEventLoop *el,
                            int fd, struct io_uring_cqe *cqe) {
    aeIOUringFdState *fs = iouringGetFdState(state, fd);

    if (!fs->active || !fs->conn) return;

    connection *conn = fs->conn;
    client *c = connGetPrivateData(conn);
    if (!c) {
        fs->active = 0;
        return;
    }

    if (cqe->res < 0) {
        serverLog(LL_VERBOSE, "io_uring write error fd=%d: %s",
                  fd, strerror(-cqe->res));
        freeClientAsync(c);
        fs->active = 0;
        return;
    }

    int nwritten = cqe->res;

    /*
     * Advance the client's sent-data tracking.
     * This mirrors the bookkeeping in _writevToClient / _writeToClientNonSlave.
     */
    if (c->bufpos > 0) {
        /* We were sending from the static reply buffer. */
        c->sentlen += nwritten;
        if (c->sentlen >= c->bufpos) {
            c->bufpos  = 0;
            c->sentlen = 0;
        }
    } else if (listLength(c->reply) > 0) {
        /* We were sending from the head reply-list block. */
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
   
    // c->io_flags &= ~CLIENT_IO_WAIT_CQE;

    /* More data to write? */
    if (clientHasPendingReplies(c)) {
        iouringSubmitClientWrite(state, el, c);
    } else {
        if (c->flags & CLIENT_CLOSE_AFTER_REPLY) {
            freeClientAsync(c);
            fs->active = 0;
            return;
        }
        iouringSubmitRecv(state, el, fd);
    }
}

static void handleCloseCQE(aeIOUringState *state, int fd) {
    if (fd < 0 || fd >= state->fd_states_size) return;
    aeIOUringFdState *fs = &state->fd_states[fd];
    fs->active = 0;
    fs->conn   = NULL;
}

/*
 * Drain io_uring completions.  Called from aeProcessEvents.
 *
 * timeout_us controls the blocking wait:
 *   >= 0: wait up to this many microseconds for at least one CQE
 *    < 0: wait indefinitely (until a CQE arrives)
 *
 * Returns the number of CQEs processed.
 */
int aeIOUringProcessCQEs(aeEventLoop *eventLoop, int64_t timeout_us) {
    aeIOUringState *state = eventLoop->iouring_state;
    if (!state || !state->initialized) return 0;

    /* Drain any CQ overflow before we add more work. */
    iouringCheckCQOverflow(state, eventLoop);

    /* Submit any pending SQEs – with SQPOLL this should not syscall. */
    io_uring_submit(&state->ring);

    struct __kernel_timespec ts;
    struct __kernel_timespec *tsp = NULL;
    if (timeout_us >= 0) {
        ts.tv_sec  = timeout_us / 1000000LL;
        ts.tv_nsec = (timeout_us % 1000000LL) * 1000LL;
        tsp = &ts;
    }

    /*
     * Choose wait_nr based on the timeout:
     *  - timeout  > 0 : we have slack before the next timer; batch up
     *    to SQ-size completions before waking.  The timeout bounds
     *    the wait so we never miss the timer.
     *  - timeout == 0 : peek only, don't block.
     *  - timeout  < 0 : no deadline; block until the first CQE
     *    arrives.  wait_nr MUST be 1 here, otherwise with tsp = NULL
     *    we could sleep forever waiting for a batch that never fills.
     * io_uring_for_each_cqe below drains everything in the CQ
     * regardless of wait_nr.
     */
    unsigned wait_nr;
    if (timeout_us > 0)        wait_nr = 128;
    else if (timeout_us == 0)  wait_nr = 0;
    else                       wait_nr = 1;

    struct io_uring_cqe *cqe = NULL;
    int ret = io_uring_wait_cqes(&state->ring, &cqe, wait_nr, tsp, NULL);
    /* -ETIME: timeout expired, -EINTR: signal, -EAGAIN: peek with
     * empty CQ (wait_nr=0).  All expected and harmless. */
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
/* Public helpers for iothread.c                                       */
/* ------------------------------------------------------------------ */

/*
 * Ensure the per-fd io_uring state on `el` is active and linked to
 * the connection.  Idempotent – safe on both fresh binds and
 * clients returning from the main thread.
 */
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

/*
 * Called by the IO thread (from processClientsFromMainThread) after
 * connRebindEventLoop has bound the connection to the IO thread's
 * event loop.  Submits the initial recv SQE so the IO thread starts
 * reading.  Idempotent.
 */
void aeIOUringClientSetup(aeEventLoop *el, client *c) {
    aeIOUringState *state = el->iouring_state;
    if (!state || !state->initialized) return;
    if (!c->conn || c->conn->fd < 0) return;

    // serverLog(LL_NOTICE, "iouring: client setup fd=%d (submit send)", c->conn->fd);
    iouringEnsureFdState(state, c);
    iouringSubmitRecv(state, el, c->conn->fd);
}

/*
 * Called by the IO thread when the main thread returns a client that
 * has pending replies to send.  Makes sure the fd_state is live on
 * this ring, then submits a send SQE for the next chunk of output.
 */
void aeIOUringClientStartWrite(aeEventLoop *el, client *c) {
    aeIOUringState *state = el->iouring_state;
    if (!state || !state->initialized) return;
    if (!c->conn || c->conn->fd < 0) return;

    // serverLog(LL_NOTICE, "iouring: start write fd=%d (submit send)", c->conn->fd);
    iouringEnsureFdState(state, c);
    iouringSubmitClientWrite(state, el, c);
}

static void iouringArmPollFd(aeIOUringState *state, aeEventLoop *el, int fd) {
    iouringCheckCQOverflow(state, el);
    struct io_uring_sqe *sqe = iouringGetSqe(state);
    io_uring_prep_poll_multishot(sqe, fd, POLLIN);
    sqe->user_data = iouringUserData(IOURING_REQ_POLL_FD, fd);
}
static void handlePollCQE(aeIOUringState *state, aeEventLoop *el,
                          int fd, struct io_uring_cqe *cqe) {
    if (fd < 0 || fd >= state->fd_states_size) return;
    // serverLog(LL_NOTICE, "iouring: pollCQE fd=%d res=%d flags=0x%x",
            //   fd, cqe->res, cqe->flags);
    aeIOUringFdState *fs = &state->fd_states[fd];
    aeIOUringFdReadyProc proc = (aeIOUringFdReadyProc)fs->poll_proc;
    if (!proc) {
        // serverLog(LL_WARNING, "iouring: pollCQE fd=%d has no handler", fd);
        return;
    }
    if (cqe->res >= 0) {
        proc(el, fd, fs->poll_data, AE_READABLE);
    }
    if (!(cqe->flags & IORING_CQE_F_MORE)) {
        iouringArmPollFd(state, el, fd);
    }
}
int aeIOUringWatchFd(aeEventLoop *el, int fd,
                     aeIOUringFdReadyProc proc, void *clientData) {
    aeIOUringState *state = el ? el->iouring_state : NULL;
    if (!state || !state->initialized) return C_ERR;
    if (fd < 0 || !proc) return C_ERR;
    aeIOUringFdState *fs = iouringGetFdState(state, fd);
    fs->poll_proc = (void*)proc;
    fs->poll_data = clientData;
    iouringArmPollFd(state, el, fd);
    return C_OK;
}

#endif /* HAVE_IO_URING */
