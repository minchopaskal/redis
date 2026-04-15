/*
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * io_uring based event processing for Redis - PoC
 */

#ifndef __AE_IOURING_H__
#define __AE_IOURING_H__

#ifdef HAVE_IO_URING

#define IOURING_SQ_ENTRIES 8192
#define IOURING_CQ_ENTRIES (IOURING_SQ_ENTRIES * 4)
#define IOURING_READBUF_SIZE (16 * 1024)

#define IOURING_REQ_ACCEPT 1
#define IOURING_REQ_READ   2
#define IOURING_REQ_WRITE  3
#define IOURING_REQ_CLOSE  4

struct aeEventLoop;
struct connection;

typedef struct aeIOUringFdState {
    char *readbuf;
    int read_len;       /* bytes available from last recv CQE */
    int read_pos;       /* current consumer position within readbuf */
    int active;
    void *conn;         /* connection* for this fd */
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

int  aeIOUringInit(struct aeEventLoop *eventLoop, int listen_fd);
void aeIOUringCleanup(struct aeEventLoop *eventLoop);
int  aeIOUringProcessCQEs(struct aeEventLoop *eventLoop);
void aeIOUringDeactivateFd(struct aeEventLoop *eventLoop, int fd);

/* io_uring connection type (defined in socket.c) */
struct ConnectionType;
struct ConnectionType *connectionTypeIOUring(void);

#endif /* HAVE_IO_URING */
#endif /* __AE_IOURING_H__ */
