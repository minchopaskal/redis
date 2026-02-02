#include "server.h"
#include "client_comp.h"
#include <zlib.h>

#define ZLIB_TEMP_BUF_SIZE 16 * 1024

typedef struct {
    unsigned char data[ZLIB_TEMP_BUF_SIZE];
    int used;
} tempBuf;

struct compressionState {
    client *client;
    tempBuf compressed;   /* Buffer holding compressed data */
    tempBuf uncompressed; /* Buffer holding uncompressed(decompressed) data */
    z_stream zlib;        /* Zlib state */
    mstime_t last_write;  /* Time since last write. Used to check if it's time
                           * to flush the buffer */
    int inflate_inited;
    int deflate_inited;
};

static void *zlib_alloc_wrapper(void *opaque, unsigned int items,
                                unsigned int size) {
    UNUSED(opaque);
    return zmalloc(items * size);
}

static void zlib_free_wrapper(void *opaque, void *address) {
    UNUSED(opaque);
    zfree(address);
}

int compressionStateCreate(client *c) {
    compressionState *st = zmalloc(sizeof(compressionState));
    st->client = c;

    st->zlib.zalloc = zlib_alloc_wrapper;
    st->zlib.zfree = zlib_free_wrapper;

    st->last_write = 0;
    st->inflate_inited = st->deflate_inited = 0;

    c->compression_state = st;

    return 1;
}

#include <stdlib.h>
void compressionStateDestroy(compressionState *state) {
    if (state == NULL) return;

    if (state->inflate_inited) {
        inflateEnd(&state->zlib);
    }
    if (state->deflate_inited) {
        deflateEnd(&state->zlib);
    }

    zfree(state);
}

int clientCreateCompressionState(client *c, compressionDirection dir) {
    /* Client compression already initialized */
    if (c->compression_state != NULL)
        return 1;

    serverAssert(compressionStateCreate(c));

    compressionState *st = c->compression_state;

    if (dir == COMPRESS) {
        serverAssert(c->compression_level > 0 && c->flags & CLIENT_SLAVE);

        if (deflateInit(&st->zlib, c->compression_level) != Z_OK) {
            serverLog(LL_NOTICE, "Failed to initialize compression: %s", st->zlib.msg);
            compressionStateDestroy(c->compression_state);
            return 0;
        }

        st->zlib.avail_out = ZLIB_TEMP_BUF_SIZE;
        st->zlib.next_out = st->compressed.data;
        st->compressed.used = 0;

        st->zlib.avail_in = 0;
        st->zlib.next_in = st->uncompressed.data;
        st->uncompressed.used = 0;

        st->deflate_inited = 1;

        serverLog(LL_NOTICE, "Initialized compression at level %d for client #%llu...",
                c->compression_level, (unsigned long long)c->id);
    } else if (dir == DECOMPRESS) {
        serverAssert(server.repl_master_compression_level > 0);

        if (inflateInit(&st->zlib) != Z_OK) {
            serverLog(LL_NOTICE, "Failed to initialize decompression: %s", st->zlib.msg);
            compressionStateDestroy(c->compression_state);
            return 0;
        }

        st->zlib.avail_out = ZLIB_TEMP_BUF_SIZE;
        st->zlib.next_out = st->uncompressed.data;
        st->compressed.used = 0;

        st->zlib.avail_in = 0;
        st->zlib.next_in = st->compressed.data;
        st->uncompressed.used = 0;

        st->inflate_inited = 1;

        serverLog(LL_NOTICE, "Decompression for master client initialized.");
    } else {
        serverAssert(0);
    }

    return 1;
}

void clientDestroyCompressionState(client *c) {
    if (c->compression_state == NULL) return;

    compressionStateDestroy(c->compression_state);
    c->compression_state = NULL;
    c->compression_level = 0;
    c->io_flags &= ~CLIENT_IO_COMPRESSION_ENABLED;

    serverLog(LL_NOTICE, "Compression state for client #%llu%s destroyed...",
              (unsigned long long)c->id,
              c->flags & CLIENT_MASTER ? " (master)" : c->flags & CLIENT_SLAVE ?
              " (slave)" : "");
}

int clientEnableCompression(client *c, compressionDirection dir) {
    if (!clientCreateCompressionState(c, dir)) {
        return 0;
    }

    c->io_flags |= CLIENT_IO_COMPRESSION_ENABLED;
    return 1;
}

void clientDisableCompression(client *c) {
    c->io_flags &= ~CLIENT_IO_COMPRESSION_ENABLED;
}

/* Write any available data in the compressed buffer. If compression_max_latency
 * has passed since last write we flush the buffer so we write to the socket
 * ASAP.
 * Return the number of bytes written to socket. Return -1 on socket error.
 * On decompression error the compression state is destroyed and -1 returned. */
int compressAndWrite(client *c) {
    if (c->compression_level <= 0)
        return 0;

    compressionState *state = c->compression_state;
    serverAssert(state);

    /* All available uncompressed data was consumed so we need to reset the
     * uncompressed buffer */
    if (state->uncompressed.used == ZLIB_TEMP_BUF_SIZE &&
        state->zlib.avail_in == 0)
    {
        state->uncompressed.used = 0;
        state->zlib.next_in = state->uncompressed.data;
    }

    if (state->zlib.avail_out > 0) {
        /* Force flush after `compression_max_latency` ms have passed.
         * Note, this only makes sense when we have enough space for compressing
         * data. */
        int flush = mstime() - state->last_write > server.compression_max_latency;
        // if (flush) serverLog(LL_NOTICE, "Flushing client %llu", c->id);
        int err = deflate(&state->zlib, flush ? Z_SYNC_FLUSH : Z_NO_FLUSH);
        if (err != Z_OK && err != Z_BUF_ERROR) {
            serverLog(LL_WARNING, "Flush compress error: %s (%d)",
                      state->zlib.msg, err);
            clientDestroyCompressionState(c);
            return -1;
        }
    }

    /* Try to write all the data available in the compressed buffer. */
    int tot_written = 0;
    int towrite =
        state->zlib.next_out - (state->compressed.data + state->compressed.used);
    while (towrite > 0) {
        int written = connWrite(
            c->conn, state->compressed.data + state->compressed.used, towrite);
        if (written < 0) {
            return written;
        }

        // if (written > 0) {
        //     char *buf = zmalloc(written + 1);
        //     memcpy(buf, state->compressed.data+state->compressed.used, written);
        //     buf[written] = 0;
        //     printf("\nWRITTEN: %s\n", buf);
        // }

        if (written > 0) {
            state->compressed.used += written;
            tot_written += written;
            towrite -= written;

        }

        /* All of the compressed data was send to the socket so we need to reset
         * the compression buffer. */
        if (state->compressed.used == ZLIB_TEMP_BUF_SIZE) {
            serverAssert(towrite == 0 && state->zlib.avail_out == 0);

            state->compressed.used = 0;
            state->zlib.next_out = state->compressed.data;
            state->zlib.avail_out = ZLIB_TEMP_BUF_SIZE;
        }
    }

    if (tot_written > 0)
        state->last_write = mstime();

    return tot_written;
}

/* Copy the data we need to send to internal buffers, pass to zlib for
 * compression and try to send as much as possible to the socket. Return how
 * much bytes we read from `data` or -1 on zlib error;
 */
int consumeAndTryWriteCompressed(client *c, char *data, size_t len,
                                 ssize_t *nwritten) {
    serverAssert(nwritten);

    compressionState *state = c->compression_state;
    serverAssert(state);

    int consumed = 0;
    while ((size_t)consumed != len) {
        int to_consume =
            min(ZLIB_TEMP_BUF_SIZE - state->uncompressed.used,
                (int)(len - consumed));

        memcpy(state->uncompressed.data + state->uncompressed.used,
               data + consumed, to_consume);

        serverAssert(to_consume >= 0);

        // if (to_consume > 0) {
        //     char *buf = zmalloc(to_consume + 1);
        //     memcpy(buf, data+consumed, to_consume);
        //     buf[to_consume] = 0;
        //     printf("\nCONSUMED: %s\n", buf);
        // }

        state->uncompressed.used += to_consume;
        consumed += to_consume;
        state->zlib.avail_in += to_consume;

        /* Write whatever we have available in the compressed buffer */
        int written = compressAndWrite(c);
        if (written < 0) {
            if (connGetState(c->conn) != CONN_STATE_CONNECTED) {
                *nwritten = -1;
                return -1;
            }
            // *nwritten = -1;
            return consumed;
        }
        *nwritten += written;

        if (written == 0)
            break;
    }

    return consumed;
}

int decompressInto(client *c, char *buf, size_t buflen) {
    if (buflen == 0)
      return 0;

    compressionState *state = c->compression_state;
    serverAssert(state);

    int consumed = 0;

    /* Decompress as much data as possible */
    while ((size_t)consumed < buflen &&
           (state->zlib.avail_in > 0 ||
            state->zlib.next_out > state->uncompressed.data + state->uncompressed.used))
    {
        /* Reset the decompressed buffer if all the available data is consumed */
        if (state->uncompressed.used == ZLIB_TEMP_BUF_SIZE &&
            state->zlib.avail_out == 0)
        {
            state->uncompressed.used = 0;
            state->zlib.next_out = state->uncompressed.data;
            state->zlib.avail_out = ZLIB_TEMP_BUF_SIZE;
        }

        if (state->zlib.avail_in > 0) {
            int err = inflate(&state->zlib, Z_SYNC_FLUSH);
            if (err != Z_OK && err != Z_BUF_ERROR) {
                serverLog(LL_NOTICE, "Decompress error: %s (%d)", state->zlib.msg, err);
                clientDestroyCompressionState(c);
                freeClientAsync(c);
                return -1;
            }
        }

        /* Copy the decompressed data to the output buffer */
        if (state->zlib.next_out >
            state->uncompressed.data + state->uncompressed.used)
        {
            int avail_decompressed =
                ZLIB_TEMP_BUF_SIZE - state->zlib.avail_out;
            int to_consume =
                min(buflen - consumed,
                    (size_t)(avail_decompressed - state->uncompressed.used));
            memcpy(buf + consumed,
                state->uncompressed.data + state->uncompressed.used, to_consume);

            // if (to_consume > 0) {
            //     char *buf = zmalloc(to_consume + 1);
            //     memcpy(buf, state->uncompressed.data+state->uncompressed.used, to_consume);
            //     buf[to_consume] = 0;
            //     printf("\nDECOMPRESSED: %s\n", buf);
            // }

            state->uncompressed.used += to_consume;
            consumed += to_consume;
        }
    }

    /* Reset the compressed buffer if we decompressed all the available data */
    if (state->compressed.used == ZLIB_TEMP_BUF_SIZE &&
        state->zlib.avail_in == 0)
    {
        state->compressed.used = 0;
        state->zlib.next_in = state->compressed.data;
    }

    serverAssert((size_t)consumed <= buflen);
    return consumed;
}

int readFromSocketAndDecompress(client *c, char *buf, size_t buflen, int *tot_read) {
    compressionState *state = c->compression_state;
    if (!state) {
        *tot_read = -1;
        return -1;
    }

    int tot_decompressed = 0;
    *tot_read = 0;

    if (c->io_flags & CLIENT_IO_READ_DECOMPRESSED_CRON &&
        (state->zlib.next_out - (state->uncompressed.data + state->uncompressed.used) <= 0))
    {
        return -1;
    }

    /* Try to read any available decompressed data */
    int written = decompressInto(c, buf, buflen);
    if (written == -1) {
        *tot_read = -1;
        return -1;
    }
    tot_decompressed += written;

    /* In case we are here because a cron function called readQueryFromClient
     * that means we only need to read any available decompressed data.
     * We don't try to read from socket in this case as this function is
     * not handling a read. */
    if (c->io_flags & CLIENT_IO_READ_DECOMPRESSED_CRON) {
        return tot_decompressed;
    }

    do {
        /* Read data from socket and save it inside the compressed buffer */
        int nread =
            connRead(c->conn, state->compressed.data + state->compressed.used,
                    ZLIB_TEMP_BUF_SIZE - state->compressed.used);

        if (nread <= 0) {
            if (nread < 0 && c->conn->state == CONN_STATE_ERROR) {
                serverLog(LL_NOTICE, "Compressed read error: %s", strerror(errno));
                clientDestroyCompressionState(c);
                freeClientAsync(c);
                *tot_read = -1;
                return -1;
            }

            /* If no error just continue - we may have some more data to
             * decompress even if the connection was closed. */
            nread = 0;
        }

        // if (nread > 0) {
        //     char *buf = zmalloc(nread + 1);
        //     memcpy(buf, state->compressed.data+state->compressed.used, nread);
        //     buf[nread] = 0;
        //     printf("\nNREAD: %s\n", buf);
        // }

        state->compressed.used += nread;
        state->zlib.avail_in += nread;
        *tot_read += nread;

        int decompressed = decompressInto(c, buf + tot_decompressed,
                                          buflen - tot_decompressed);
        if (decompressed <= 0)
            break;
        tot_decompressed += decompressed;
    } while ((size_t)tot_decompressed < buflen && state->zlib.avail_in > 0);

    if (*tot_read == 0 && c->conn->state == CONN_STATE_CONNECTED)
        *tot_read = -1;

    return tot_decompressed;
}

int readFromBufAndDecompress(struct client *c, char *input_buf, size_t input_len,
                             char *output_buf, size_t output_len, size_t *consumed)
{
    compressionState *state = c->compression_state;
    if (!state) {
        return -1;
    }

    int tot_decompressed = 0;
    *consumed = 0;
    while (*consumed < input_len && (size_t)tot_decompressed < output_len) {
        int to_consume =
            min(ZLIB_TEMP_BUF_SIZE - state->compressed.used,
                (int)(input_len - *consumed));

        memcpy(state->compressed.data + state->compressed.used,
               input_buf + *consumed, to_consume);

        *consumed += to_consume;

        state->compressed.used += to_consume;
        state->zlib.avail_in += to_consume;

        int decompressed = decompressInto(c, output_buf + tot_decompressed,
                                          output_len - tot_decompressed);
        if (decompressed <= 0)
            break;
        tot_decompressed += decompressed;
    }

    return tot_decompressed;

}

