/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef __CLIENT_COMP_H
#define __CLIENT_COMP_H

#include <sys/types.h>

/* Opaque handle to client's compression state used internally by client_comp */
typedef struct compressionState compressionState;

struct client;

typedef enum {
    COMPRESS,
    DECOMPRESS,
} compressionDirection;

int clientCreateCompressionState(struct client *c, compressionDirection dir);
void clientDestroyCompressionState(struct client *c);

/* Enable client compression and create compression state if not present.
 * Currently only valid for primary/replica clients.
 * Return 0 if compression state was not created and failed to be initialized,
 * 1 if compression was enabled. */
int clientEnableCompression(struct client *c, compressionDirection dir);

/* Disable compression without destroying compression state. If compression was
 * not initialized does nothing. */
void clientDisableCompression(struct client *c);

/* Compress any data send for compression by consumeAndTryWriteCompressed and
 * write to socket. Zlib may not return compressed data immediately so this
 * call may not write anything to socket.
 * Force flushes the compressed buffer according to compression_max_latency.
 * Return number of bytes written to socket or -1 on socket write error. */
int compressAndWrite(struct client *c);

/* Consume bytes from the `data` buffer, then try to compress and write to the
 * client's connection. Zlib may not return compressed data immediately so this
 * call may not write anything to socket.
 * Return number of bytes consumed from `data`. `nwritten` must be a valid
 * pointer - it is incremented by the number of bytes written to socket or set
 * to -1 on socket write error. */
int consumeAndTryWriteCompressed(struct client *c, char *data, size_t len,
                                 ssize_t *nwritten);

/* Read data from client connection and decompress it immediately. The result is
 * written into buf.
 * Return number of bytes decompressed. *nread stores number of bytes read from
 * the socket or -1 on socket read error.
 * Note, that we may have enough compressed data on the socket that decompressing
 * it will exceed buflen. To handle decompressed data without readQueryFromClient
 * being called from a read handler, the readQueryFromClient is called in an
 * IO-thread cron with the client->flag & CLIENT_IO_READ_DECOMPRESSED_CRON. With
 * that flag raised we don't read from socket - we only read any available
 * decompressed data. */
int readFromSocketAndDecompress(struct client *c, char *buf, size_t buflen,
                                int *nread);

/* Same as readFromSocketAndDecompress but reads from `input_buf` instead of
 * socket. */
int readFromBufAndDecompress(struct client *c, char *input_buf, size_t input_len,
                             char *output_buf, size_t output_len,
                             size_t *consumed);

#endif /* __CLIENT_COMP_H */
