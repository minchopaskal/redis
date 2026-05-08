/*
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * io_uring based event processing for Redis - PoC
 */

#ifndef __AE_IOURING_H__
#define __AE_IOURING_H__

#ifdef HAVE_IO_URING

#include <stdint.h>

struct aeEventLoop;
struct connection;

typedef void (*aeIOUringFdReadyProc)(struct aeEventLoop *el, int fd,
                                     void *clientData, int mask);

typedef struct aeIOUringFdState {
    char *readbuf;
    int read_len;       /* bytes available from last recv CQE */
    int read_pos;       /* current consumer position within readbuf */
    int active;
    void *conn;         /* connection* for this fd */

    /* Poll-watch path (e.g. IO-thread notifier eventfds) */
    aeIOUringFdReadyProc poll_proc;
    void *poll_data;
    uint64_t ev_buf;        /* notifier read scratch */
} aeIOUringFdState;

/*
 * Look up the per-fd io_uring state for a connection.  Called from the
 * io_uring connection-type read method (connIOUringRead) so that connRead()
 * serves data from the pre-filled io_uring buffer instead of calling read(2).
 * Returns NULL if this fd has no io_uring state.
 */
aeIOUringFdState *aeIOUringGetFdState(struct aeEventLoop *el, int fd);

/*
 * Opaque io_uring state. Full definition is in ae_iouring.c because
 * we don't want to leak the liburing header into every translation unit.
 */
typedef struct aeIOUringState aeIOUringState;

/*
 * Initialize io_uring on an event loop.
 *   listen_fd >= 0 : main-thread ring; sets up multishot accept on listen_fd.
 *   listen_fd <  0 : IO-thread ring; no accept, handles only its clients.
 */
int  aeIOUringInit(struct aeEventLoop *eventLoop, int listen_fd, int max_bgworkers);
void aeIOUringCleanup(struct aeEventLoop *eventLoop);
int  aeIOUringProcessCQEs(struct aeEventLoop *eventLoop, int64_t timeout_us);
void aeIOUringDeactivateFd(struct aeEventLoop *eventLoop, int fd);

/*
 * Public helpers used by iothread.c when a client transitions from the
 * main thread to an IO thread (via processClientsFromMainThread):
 *
 *   aeIOUringClientSetup       – submit the initial recv SQE on the
 *     target event loop's ring and link the fd_state to this
 *     connection.  Called after connRebindEventLoop on the IO thread.
 *
 *   aeIOUringClientStartWrite  – submit a send SQE for the next chunk
 *     of the client's pending reply on the target event loop's ring.
 *     Used when main returns a client with pending replies.
 */
struct client;
void aeIOUringClientSetup(struct aeEventLoop *el, struct client *c);
void aeIOUringClientStartWrite(struct aeEventLoop *el, struct client *c);

int aeIOUringSetupReadEventNotifier(struct aeEventLoop *el, int fd,
                                    aeIOUringFdReadyProc proc, void *clientData);

/* io_uring connection type (defined in socket.c) */
struct ConnectionType;
struct ConnectionType *connectionTypeIOUring(void);

#endif /* HAVE_IO_URING */
#endif /* __AE_IOURING_H__ */
