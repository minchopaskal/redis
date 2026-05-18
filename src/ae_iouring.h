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

#define CLIENT_IO_WAIT_RECV_CQE (1ULL<<0)
#define CLIENT_IO_WAIT_WRITE_CQE (1ULL<<1)

#define CLIENT_IO_WAIT_CQE (CLIENT_IO_WAIT_RECV_CQE | CLIENT_IO_WAIT_WRITE_CQE)

struct aeEventLoop;
struct connection;

typedef void (*aeIOUringEventNotifierProc)(struct aeEventLoop *el, int fd,
                                           void *clientData, int mask);

typedef struct aeIOUringFdState {
    char *readbuf;
    int read_len;
    int read_pos;
    int active;
    void *conn;

    aeIOUringEventNotifierProc evn_proc;
    void *evn_data;
    uint64_t evn_buf;
} aeIOUringFdState;

aeIOUringFdState *aeIOUringGetFdState(struct aeEventLoop *el, int fd);

typedef struct aeIOUringState aeIOUringState;

int  aeIOUringInit(struct aeEventLoop *eventLoop, int listen_fd, int max_bgworkers);
void aeIOUringCleanup(struct aeEventLoop *eventLoop);
int  aeIOUringProcessCQEs(struct aeEventLoop *eventLoop, int64_t timeout_us);
void aeIOUringDeactivateFd(struct aeEventLoop *eventLoop, int fd);

struct client;
void aeIOUringClientSetup(struct aeEventLoop *el, struct client *c);
void aeIOUringClientStartWrite(struct aeEventLoop *el, struct client *c);

int aeIOUringSetupReadEventNotifier(struct aeEventLoop *el, int fd,
                                    aeIOUringEventNotifierProc proc, void *clientData);

struct ConnectionType;
struct ConnectionType *connectionTypeIOUring(void);

#endif /* HAVE_IO_URING */
#endif /* __AE_IOURING_H__ */
