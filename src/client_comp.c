#include "server.h"
#include "client_comp.h"
#include <zlib.h>
#include <zstd.h>

#define ZLIB_TEMP_BUF_SIZE 16 * 1024

typedef struct compressionType {
    int (*init_compress)(struct compressionState *st, int level);
    int (*init_decompress)(struct compressionState *st);
    int (*compress)(struct compressionState *st, int flush);
    int (*decompress)(struct compressionState *st);
    void (*end)(struct compressionState *st);
} compressionType;

typedef struct {
    unsigned char *data;
    int size;
    int written;
    int consumed;
} tempBuf;

/* Main compression state struct */
struct compressionState {
    const compressionType *type;
    tempBuf input;  /* Buffer holding compressed data */
    tempBuf output; /* Buffer holding uncompressed(decompressed) data */
    union {
        z_stream zlib;            /* Zlib state */
        ZSTD_CStream *zstdCCtx;  /* Zstd compression ctx */
        ZSTD_DStream *zstdDCtx;  /* Zstd decompression ctx */
    };
    int write_flush_pending;    /* zstd: flush not yet completed, keep calling with ZSTD_e_flush */
    int read_flush_pending;    /* zstd: flush not yet completed, keep calling with ZSTD_e_flush */
    mstime_t last_write;  /* Time since last write. Used to check if it's time
                           * to flush the buffer */
    compressionDirection dir;
};

/* --- zlib --- */
static void *zlib_alloc_wrapper(void *opaque, unsigned int items,
                                unsigned int size) {
    UNUSED(opaque);
    return zmalloc(items * size);
}

static void zlib_free_wrapper(void *opaque, void *address) {
    UNUSED(opaque);
    zfree(address);
}

static int zlibInitCompress(compressionState *st, int level) {
    st->zlib.zalloc = zlib_alloc_wrapper;
    st->zlib.zfree = zlib_free_wrapper;

    if (deflateInit(&st->zlib, level) != Z_OK) {
        serverLog(LL_NOTICE, "Failed to initialize zlib compression: %s", st->zlib.msg);
        return -1;
    }

    st->output.data = zmalloc(ZLIB_TEMP_BUF_SIZE);
    st->output.size = ZLIB_TEMP_BUF_SIZE;
    st->output.written = 0;
    st->output.consumed = 0;
    st->zlib.avail_out = ZLIB_TEMP_BUF_SIZE;
    st->zlib.next_out = st->output.data;

    st->input.data = zmalloc(ZLIB_TEMP_BUF_SIZE);
    st->input.size = ZLIB_TEMP_BUF_SIZE;
    st->input.written = 0;
    st->input.consumed = 0;
    st->zlib.avail_in = 0;
    st->zlib.next_in = st->input.data;

    return 0;
}

static int zlibInitDecompress(compressionState *st) {
    st->zlib.zalloc = zlib_alloc_wrapper;
    st->zlib.zfree = zlib_free_wrapper;
    if (inflateInit(&st->zlib) != Z_OK) {
        serverLog(LL_NOTICE, "Failed to initialize zlib decompression: %s", st->zlib.msg);
        return -1;
    }

    st->input.data = zmalloc(ZLIB_TEMP_BUF_SIZE);
    st->input.size = ZLIB_TEMP_BUF_SIZE;
    st->input.written = 0;
    st->input.consumed = 0;
    st->zlib.avail_in = 0;
    st->zlib.next_in = st->input.data;

    st->output.data = zmalloc(ZLIB_TEMP_BUF_SIZE);
    st->output.size = ZLIB_TEMP_BUF_SIZE;
    st->output.written = 0;
    st->output.consumed = 0;
    st->zlib.avail_out = ZLIB_TEMP_BUF_SIZE;
    st->zlib.next_out = st->output.data;

    return 0;
}

static int zlibCompress(compressionState *st, int flush) {
    st->zlib.next_out = st->output.data + st->output.written;
    st->zlib.avail_out = st->output.size - st->output.written;
    st->zlib.next_in = st->input.data + st->input.consumed;
    st->zlib.avail_in =  st->input.written - st->input.consumed;

    int err = deflate(&st->zlib, flush ? Z_SYNC_FLUSH : Z_NO_FLUSH);
    if (err != Z_OK && err != Z_BUF_ERROR) {
        serverLog(LL_WARNING, "zlib compress error: %s (%d)", st->zlib.msg, err);
        return -1;
    }

    st->output.written = st->zlib.next_out - st->output.data;
    st->input.consumed = st->zlib.next_in - st->input.data;

    return 0;
}

static int zlibDecompress(compressionState *st) {
    st->zlib.next_in = st->input.data + st->input.consumed;
    st->zlib.avail_in = st->input.written - st->input.consumed;
    st->zlib.next_out = st->output.data + st->output.written;
    st->zlib.avail_out = st->output.size - st->output.written;

    int err = inflate(&st->zlib, Z_SYNC_FLUSH);
    if (err != Z_OK && err != Z_BUF_ERROR) {
        serverLog(LL_NOTICE, "zlib decompress error: %s (%d)", st->zlib.msg, err);
        return -1;
    }

    st->output.written = st->zlib.next_out - st->output.data;
    st->input.consumed = st->zlib.next_in - st->input.data;

    return 0;
}

static void zlibEnd(compressionState *st) {
    if (st->dir == COMPRESS)
        deflateEnd(&st->zlib);
    else if (st->dir == DECOMPRESS)
        inflateEnd(&st->zlib);
}

static const compressionType zlibType = {
    .init_compress = zlibInitCompress,
    .init_decompress = zlibInitDecompress,
    .compress = zlibCompress,
    .decompress = zlibDecompress,
    .end = zlibEnd,
};

/* --- zstd --- */

static int zstdInitCompress(compressionState *st, int level) {
    st->zstdCCtx = ZSTD_createCStream();
    if (!st->zstdCCtx) {
        serverLog(LL_NOTICE, "Failed to create ZSTD compression context");
        return -1;
    }
    ZSTD_CCtx_setParameter(st->zstdCCtx, ZSTD_c_compressionLevel, level);

    size_t outSize = ZSTD_CStreamOutSize();
    st->output.data = zmalloc(outSize);
    st->output.size = outSize;
    st->output.written = 0;
    st->output.consumed = 0;

    size_t inSize = ZSTD_CStreamInSize();
    st->input.data = zmalloc(inSize);
    st->input.size = inSize;
    st->input.written = 0;
    st->input.consumed = 0;

    st->write_flush_pending = 0;

    return 0;
}

static int zstdInitDecompress(compressionState *st) {
    st->zstdDCtx = ZSTD_createDStream();
    if (!st->zstdDCtx) {
        serverLog(LL_NOTICE, "Failed to create ZSTD decompression context");
        return -1;
    }

    size_t inSize = ZSTD_DStreamInSize();
    st->input.data = zmalloc(inSize);
    st->input.size = inSize;
    st->input.written = 0;
    st->input.consumed = 0;

    size_t outSize = ZSTD_DStreamOutSize();
    st->output.data = zmalloc(outSize);
    st->output.size = outSize;
    st->output.written = 0;
    st->output.consumed = 0;

    st->read_flush_pending = 0;

    return 0;
}

static int zstdCompress(compressionState *st, int flush) {
    ZSTD_inBuffer input = {
        .src  = st->input.data,
        .size = st->input.written,
        .pos  = st->input.consumed
    };
    ZSTD_outBuffer output = {
        .dst  = st->output.data,
        .size = st->output.size,
        .pos  = st->output.written
    };

    ZSTD_EndDirective directive;
    if (flush || st->write_flush_pending)
        directive = ZSTD_e_flush;
    else
        directive = ZSTD_e_continue;

    size_t ret;
    do {
        ret = ZSTD_compressStream2(st->zstdCCtx, &output, &input, directive);
        if (ZSTD_isError(ret)) {
            serverLog(LL_WARNING, "zstd compress error: %s", ZSTD_getErrorName(ret));
            return -1;
        }
    } while (directive == ZSTD_e_flush && ret > 0 && output.pos < output.size);

    st->write_flush_pending = (directive == ZSTD_e_flush && ret > 0);

    st->input.consumed = input.pos;
    st->output.written = output.pos;

    return 0;
}

static int zstdDecompress(compressionState *st) {
    ZSTD_inBuffer input = {
        .src  = st->input.data,
        .size = st->input.written,
        .pos  = st->input.consumed
    };
    ZSTD_outBuffer output = {
        .dst  = st->output.data,
        .size = st->output.size,
        .pos  = st->output.written
    };

    size_t ret = ZSTD_decompressStream(st->zstdDCtx, &output, &input);
    if (ZSTD_isError(ret)) {
        serverLog(LL_NOTICE, "zstd decompress error: %s", ZSTD_getErrorName(ret));
        return -1;
    }

    /* Don't try to flush again if we already tried, no more progress can be
     * made without additional input. */
    st->read_flush_pending = !st->read_flush_pending && (ret > 0);

    st->input.consumed = input.pos;
    st->output.written = output.pos;

    return 0;
}

static void zstdEnd(compressionState *st) {
    if (st->dir == COMPRESS && st->zstdCCtx)
        ZSTD_freeCStream(st->zstdCCtx);
    else if (st->dir == DECOMPRESS && st->zstdDCtx)
        ZSTD_freeDStream(st->zstdDCtx);
}

static const compressionType zstdType = {
    .init_compress = zstdInitCompress,
    .init_decompress = zstdInitDecompress,
    .compress = zstdCompress,
    .decompress = zstdDecompress,
    .end = zstdEnd,
};

/* Compression connection */
typedef struct compressionConnection {
    connection base;
    connection *underlying;
    listNode *pending_data_node;
    size_t last_read;
    size_t last_written;
    int handle_pending;
} compressionConnection;

int decompressInto(compressionState *state, char *buf, size_t buflen);

static void compressionPendingAdd(compressionConnection *cc) {
    if (cc->pending_data_node) return;

    if (!cc->underlying->el->privdata[2])
        cc->underlying->el->privdata[2] = listCreate();

    list *l = cc->underlying->el->privdata[2];
    listAddNodeTail(l, cc);
    cc->pending_data_node = listLast(l);
}

static void compressionPendingRemove(compressionConnection *cc) {
    list *l = cc->underlying->el->privdata[2];
    if (!l || listLength(l) == 0) return;
    listDelNode(l, cc->pending_data_node);
    cc->pending_data_node = NULL;
}

static const char *connCompressionGetType(connection *conn) {
    UNUSED(conn);
    return CONN_TYPE_COMPRESSION;
}

static void connCompressionAeHandler(aeEventLoop *el, int fd, void *clientData, int mask) {
    compressionConnection *cc = (compressionConnection *)clientData;
    cc->underlying->type->ae_handler(el, fd, clientData, mask);
}

static int connCompressionAddr(connection *conn, char *ip, size_t ip_len, int *port, int remote) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->type->addr(cc->underlying, ip, ip_len, port, remote);
}

static int connCompressionIsLocal(connection *conn) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->type->is_local(cc->underlying);
}

static void connCompressionShutdown(connection *conn) {
    compressionConnection *cc = (compressionConnection*)conn;
    cc->underlying->type->shutdown(cc->underlying);
}

static void connCompressionClose(connection *conn) {
    compressionConnection *cc = (compressionConnection*)conn;
    cc->underlying->type->close(cc->underlying);
    zfree(conn);
}

static int connCompressionConnect(connection *conn, const char *addr, int port, const char *source_addr, ConnectionCallbackFunc connect_handler) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->type->connect(cc->underlying, addr, port, source_addr, connect_handler);
}

static int connCompressionBlockingConnect(connection *conn, const char *addr, int port, long long timeout) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->type->blocking_connect(cc->underlying, addr, port, timeout);
}

static int connCompressionAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->type->accept(cc->underlying, accept_handler);
}

static int connCompressionRead(struct connection *conn, void *buf, size_t buf_len) {
    compressionConnection *cc = (compressionConnection*)conn;
    client *c = connGetPrivateData(conn);
    compressionState *state = c->compression_state;
    if (!state || state->dir != DECOMPRESS)
        return connRead(cc->underlying, buf, buf_len);

    cc->last_read = 0;
    size_t decompressed = 0;
    do {
        int curr = decompressInto(state, (char*)buf + decompressed, buf_len - decompressed);
        /* Decompression error, we should close the connection */
        if (curr < 0) {
            cc->underlying->state = CONN_STATE_CLOSED;
            break;
        }
        decompressed += curr;

        int nread = 0;
        /* If the handle_pending flag is raised we only decompress whatever data
         * we have read from the socket without reading anything more. Socket
         * reading will happened when the event loop handles read event in which
         * case the handle_pending flags wouldn't be raised. */
        if (!cc->handle_pending) {
            nread = cc->underlying->type->read(
                cc->underlying,
                state->input.data + state->input.written,
                state->input.size - state->input.written);

            // if (nread > 0) {
            //     char *buf = zmalloc(nread + 1);
            //     memcpy(buf, state->input.data+state->input.written, nread);
            //     buf[nread] = 0;
            //     printf("\nNREAD: %s\n", buf);
            //     zfree(buf);
            // }

            if (nread < 0 && connGetState(cc->underlying) == CONN_STATE_ERROR) {
                cc->last_read = -1;
                return -1;
            }
            /* Even if nread == 0 we continue the loop until decompressInto has
             * nothing more it can do. */
            if (nread > 0) {
                cc->last_read += nread;
                state->input.written += nread;
            }
        }

        if (curr <= 0 && nread <= 0) break;
    } while (decompressed <= buf_len);

    if (decompressed == 0 && connGetState(cc->underlying) == CONN_STATE_CONNECTED)
        return -1;

    if (connGetState(conn) == CONN_STATE_CONNECTED && clientHasPendingCompressedData(c)) {
        compressionPendingAdd(cc);
    } else {
        compressionPendingRemove(cc);
    }

    return decompressed;
}

static int connCompressionWrite(connection *conn, const void *data, size_t len) {
    compressionConnection *cc = (compressionConnection*)conn;
    client *c = connGetPrivateData(conn);
    compressionState *state = c->compression_state;
    /* If no compression state or we are not compressing data - defer to
     * underlying connection. */
    if (!state || state->dir != COMPRESS)
        return connWrite(cc->underlying, data, len);

    int consumed = 0;
    cc->last_written = 0;
    while ((size_t)consumed != len) {
        int to_consume =
            min(state->input.size - state->input.written, (int)(len - consumed));
        serverAssert(to_consume >= 0);

        memcpy(state->input.data + state->input.written,
               (char*)data + consumed, to_consume);

        // if (to_consume > 0) {
        //     char *buf = zmalloc(to_consume + 1);
        //     memcpy(buf, (char*)data+consumed, to_consume);
        //     buf[to_consume] = 0;
        //     printf("\nCONSUMED: %s\n", buf);
        //     zfree(buf);
        // }

        state->input.written += to_consume;
        consumed += to_consume;

        /* Write whatever we have available in the compressed buffer */
        int written = 0;
        int err = compressAndWrite(c, &written);
        cc->last_written += written;
        if (err) {
            if (connGetState(c->conn) != CONN_STATE_CONNECTED) {
                return -1;
            }
            return consumed;
        }

        if (written == 0 && state->output.written == state->output.consumed)
            break;
    }

    return consumed;
}

static int connCompressionWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->type->writev(cc->underlying, iov, iovcnt);
}

static int connCompressionSetWriteHandler(connection *conn, ConnectionCallbackFunc handler, int barrier) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->type->set_write_handler(cc->underlying, handler, barrier);
}

static int connCompressionSetReadHandler(connection *conn, ConnectionCallbackFunc handler) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->type->set_read_handler(cc->underlying, handler);
}

static const char *connCompressionGetLastError(connection *conn) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->type->get_last_error(cc->underlying);
}

static ssize_t connCompressionSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->type->sync_write(cc->underlying, ptr, size, timeout);
}

static ssize_t connCompressionSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->type->sync_read(cc->underlying, ptr, size, timeout);
}

static ssize_t connCompressionSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->type->sync_readline(cc->underlying, ptr, size, timeout);
}

static size_t connCompressionGetLastRead(connection *conn) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->last_read;
}

static size_t connCompressionGetLastWritten(connection *conn) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->last_written;
}

static void connCompressionUnbindEventLoop(connection *conn) {
    compressionConnection *cc = (compressionConnection*)conn;
    aeEventLoop *el = cc->underlying->el;
    if (el) {
        int fd = cc->underlying->fd;
        int mask = aeGetFileEvents(el, cc->underlying->fd);
        if (mask & AE_READABLE) aeDeleteFileEvent(el, fd, AE_READABLE);
        if (mask & AE_WRITABLE) aeDeleteFileEvent(el, fd, AE_WRITABLE);
    }
    if (cc->pending_data_node) {
        compressionPendingRemove(cc);
    }
}

static int connCompressionRebindEventLoop(connection *conn, aeEventLoop *el) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->type->rebind_event_loop(cc->underlying, el);
}

static aeEventLoop *connCompressionGetEventLoop(connection *conn) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->el;
}

static void connCompressionUnsetEventLoop(connection *conn) {
    compressionConnection *cc = (compressionConnection*)conn;
    cc->underlying->el = NULL;
}

static int connCompressionGetFd(connection *conn) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->fd;
}

static int connCompressionGetIovcnt(connection *conn) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->iovcnt;
}

static ConnectionState connCompressionGetState(connection *conn) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->state;
}

static int connCompressionGetLastErrno(connection *conn) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->last_errno;
}

static int connCompressionHasReadHandler(connection *conn) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->read_handler != NULL;
}

static int connCompressionHasWriteHandler(connection *conn) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->write_handler != NULL;
}

static void connCompressionSetPrivateData(connection *conn, void *data) {
    compressionConnection *cc = (compressionConnection*)conn;
    cc->underlying->private_data = data;
}

static void *connCompressionGetPrivateData(connection *conn) {
    compressionConnection *cc = (compressionConnection*)conn;
    return cc->underlying->private_data;
}

static int compressionHasPendingData(struct aeEventLoop *el) {
    list *pending_list = el->privdata[2];
    if (!pending_list)
        return 0;
    return listLength(pending_list) > 0;
}

static int compressionProcessPendingData(struct aeEventLoop *el) {
    list *pending_list = el->privdata[2];
    if (!pending_list || listLength(pending_list) == 0)
        return 0;

    listIter li;
    listNode *ln;
    int processed = 0;
    listRewind(pending_list,&li);
    while((ln = listNext(&li))) {
        compressionConnection *cc = listNodeValue(ln);
        if (!cc || !connHasReadHandler((connection*)cc)) continue;

        cc->handle_pending = 1;
        cc->underlying->read_handler(cc->underlying);
        cc->handle_pending = 0;

        ++processed;
    }
    return processed;
}

/* Compression connection */
static ConnectionType CT_Compression = {
    /* connection type */
    .get_type = connCompressionGetType,

    /* ae & accept & listen & error & address handler */
    .ae_handler = connCompressionAeHandler,
    .addr = connCompressionAddr,
    .is_local = connCompressionIsLocal,

    /* shutdown/close connection */
    .shutdown = connCompressionShutdown,
    .close = connCompressionClose,

    /* connect & accept */
    .connect = connCompressionConnect,
    .blocking_connect = connCompressionBlockingConnect,
    .accept = connCompressionAccept,

    /* event loop */
    .unbind_event_loop = connCompressionUnbindEventLoop,
    .rebind_event_loop = connCompressionRebindEventLoop,

    /* IO */
    .read = connCompressionRead,
    .write = connCompressionWrite,
    .writev = connCompressionWritev,
    .set_write_handler = connCompressionSetWriteHandler,
    .set_read_handler = connCompressionSetReadHandler,
    .get_last_error = connCompressionGetLastError,
    .sync_write = connCompressionSyncWrite,
    .sync_read = connCompressionSyncRead,
    .sync_readline = connCompressionSyncReadLine,
    .get_last_read = connCompressionGetLastRead,
    .get_last_written = connCompressionGetLastWritten,

    /* pending data */
    .has_pending_data = compressionHasPendingData,
    .process_pending_data = compressionProcessPendingData,

    .get_peer_cert = NULL,
    .get_peer_username = NULL,

    /* connection accessors */
    .get_event_loop = connCompressionGetEventLoop,
    .unset_event_loop = connCompressionUnsetEventLoop,
    .get_fd = connCompressionGetFd,
    .get_iovcnt = connCompressionGetIovcnt,
    .get_state = connCompressionGetState,
    .get_last_errno = connCompressionGetLastErrno,
    .has_read_handler = connCompressionHasReadHandler,
    .has_write_handler = connCompressionHasWriteHandler,
    .set_private_data = connCompressionSetPrivateData,
    .get_private_data = connCompressionGetPrivateData,
};

int RedisRegisterConnectionTypeCompression(void) {
    return connTypeRegister(&CT_Compression);
}

int compressionStateCreate(client *c) {
    compressionState *st = zmalloc(sizeof(compressionState));
    st->type = &zstdType;
    st->last_write = 0;
    st->write_flush_pending = 0;
    st->read_flush_pending = 0;
    st->dir = CD_INVALID;

    c->compression_state = st;

    return 1;
}

void compressionStateDestroy(compressionState *state) {
    if (state == NULL) return;

    state->type->end(state);
    zfree(state->input.data);
    zfree(state->output.data);
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

        if (st->type->init_compress(st, c->compression_level) == -1) {
            compressionStateDestroy(c->compression_state);
            return 0;
        }

        st->dir = COMPRESS;

        serverLog(LL_NOTICE, "Initialized compression at level %d for client #%llu...",
                c->compression_level, (unsigned long long)c->id);
    } else if (dir == DECOMPRESS) {
        serverAssert(server.repl_master_compression_level > 0);

        if (st->type->init_decompress(st) == -1) {
            compressionStateDestroy(c->compression_state);
            return 0;
        }
 
        st->dir = DECOMPRESS;

        serverLog(LL_NOTICE, "Decompression for master client initialized.");
    } else {
        serverAssert(0);
    }

    compressionConnection *cc = zmalloc(sizeof(compressionConnection));
    cc->base.type = &CT_Compression;
    cc->underlying = c->conn;
    cc->pending_data_node = NULL;
    cc->last_read = 0;
    cc->last_written = 0;
    cc->handle_pending = 0;
    c->conn = (connection*)cc;

    return 1;
}

void clientDestroyCompressionState(client *c) {
    if (c->compression_state == NULL) return;

    if (c->conn) {
        compressionConnection *cc = (compressionConnection*)c->conn;
        c->conn = cc->underlying;

        zfree(cc);
    }

    compressionStateDestroy(c->compression_state);
    c->compression_state = NULL;
    c->compression_level = 0;
    c->io_flags &= ~CLIENT_IO_COMPRESSION_ENABLED;

    serverLog(LL_NOTICE, "Compression state for client #%llu%s destroyed...",
              (unsigned long long)c->id,
              c->flags & CLIENT_MASTER ? " (master)" : c->flags & CLIENT_SLAVE ?
              " (slave)" : "");
}

/* Enable client compression and create compression state if not present.
 * Currently only valid for primary/replica clients.
 * Return 0 if compression state was not created and failed to be initialized,
 * 1 if compression was enabled. */
int clientEnableCompression(client *c, compressionDirection dir) {
    if (!clientCreateCompressionState(c, dir)) {
        return 0;
    }

    c->io_flags |= CLIENT_IO_COMPRESSION_ENABLED;
    return 1;
}

/* Disable compression without destroying compression state. If compression was
 * not initialized does nothing. */
void clientDisableCompression(client *c) {
    c->io_flags &= ~CLIENT_IO_COMPRESSION_ENABLED;
}

/* Compress any data send for compression (see connCompressionWrite) and
 * write to socket. Zlib may not return compressed data immediately so this
 * call may not write anything to socket.
 * Force flushes the compressed buffer according to compression_max_latency.
 * Return number of bytes written to socket or -1 on socket write error. */
int compressAndWrite(client *c, int *tot_written) {
    if (c->compression_level <= 0)
        return 0;

    compressionState *state = c->compression_state;
    serverAssert(state);

    /* All available uncompressed data was consumed so we need to reset the
     * uncompressed buffer */
    if (state->input.written == state->input.size &&
        state->input.consumed == state->input.size)
    {
        state->input.written = 0;
        state->input.consumed = 0;
    }

    if (state->output.written < state->output.size) {
        /* Force flush after `compression_max_latency` ms have passed.
         * Note, this only makes sense when we have enough space for compressing
         * data. */
        int flush = mstime() - state->last_write > server.compression_max_latency;
        // if (flush) serverLog(LL_NOTICE, "Flushing client %llu", c->id);
        if (state->type->compress(state, flush) == -1) {
            clientDestroyCompressionState(c);
            return 1;
        }
    }

    compressionConnection *cc = (compressionConnection*)c->conn;

    /* Try to write all the data available in the compressed buffer. */
    *tot_written = 0;
    int towrite =
        state->output.written - state->output.consumed;
    do {
        int written = connWrite(
            cc->underlying, state->output.data + state->output.consumed, towrite);
        if (written < 0) {
            return 1;
        }

        // if (written > 0) {
        //     char *buf = zmalloc(written + 1);
        //     memcpy(buf, state->output.data+state->output.consumed, written);
        //     buf[written] = 0;
        //     printf("WRITTEN COMPRESSED: %s\n", buf);
        //     zfree(buf);
        // }

        state->output.consumed += written;
        *tot_written += written;
        towrite -= written;

        /* All of the compressed data was send to the socket so we need to reset
         * the compression buffer. */
        if (state->output.consumed == state->output.size) {
            serverAssert(towrite == 0 && state->output.written == state->output.size);

            state->output.written = 0;
            state->output.consumed = 0;
        }
    } while (towrite > 0);

    if (*tot_written > 0)
        state->last_write = mstime();

    return 0;
}

int decompressInto(compressionState *state, char *buf, size_t buflen) {
    if (buflen == 0)
      return 0;

    int consumed = 0;

    /* Decompress as much data as possible */
    while ((size_t)consumed < buflen &&
           (state->read_flush_pending ||
            state->input.written > state->input.consumed ||
            state->output.written > state->output.consumed))
    {
        /* Reset the decompressed buffer if all the available data is consumed */
        if (state->output.consumed == state->output.size) {
            state->output.written = 0;
            state->output.consumed = 0;
        }

        if ((state->read_flush_pending && state->output.size > state->output.written) ||
            state->input.written > state->input.consumed)
        {
            if (state->type->decompress(state) == -1) {
                return -1;
            }
        }

        /* Copy the decompressed data to the output buffer */
        if (state->output.written > state->output.consumed) {
            size_t nonconsumed_decompressed = state->output.written - state->output.consumed;
            int to_consume = min(buflen - consumed, nonconsumed_decompressed);
            memcpy(buf + consumed, state->output.data + state->output.consumed, to_consume);

            // if (to_consume > 0) {
            //     char *res = zmalloc(to_consume + 1);
            //     memcpy(res, buf+consumed, to_consume);
            //     res[to_consume] = 0;
            //     printf("\nDECOMPRESSED: %s\n", res);
            //     zfree(res);
            // }

            state->output.consumed += to_consume;
            consumed += to_consume;
        }
    }

    if (state->output.consumed == state->output.size)
    {
        state->output.written = 0;
        state->output.consumed = 0;
    }

    /* Reset the compressed buffer if we decompressed all the available data */
    if (state->input.consumed == state->input.size) {
        state->input.written = 0;
        state->input.consumed = 0;
    }

    serverAssert((size_t)consumed <= buflen);
    return consumed;
}

/* Read data from input_buf and decompress it immediately. The result is written
 * into output_buf.
 * Return number of bytes decompressed. *consumed stores number of bytes consumed
 * from input_buf.
 * Note, that we may have enough compressed data inside input buf so that decompressing
 * it will exceed output_len. The function must be ran in a loop until input_buf
 * is fully consumed - so make sure to have free space in output_buf on each call. */
int readFromBufAndDecompress(client *c, char *input_buf, size_t input_len,
                             char *output_buf, size_t output_len, size_t *consumed)
{
    compressionState *state = c->compression_state;
    if (!state) {
        return -1;
    }

    int tot_decompressed = 0;
    *consumed = 0;
    while (*consumed <= input_len && (size_t)tot_decompressed < output_len) {
        int to_consume =
            min(state->input.size - state->input.written,
                (int)(input_len - *consumed));

        if (to_consume)
            memcpy(state->input.data + state->input.written,
                   input_buf + *consumed, to_consume);

        // if (to_consume > 0) {
        //     char *buf = zmalloc(to_consume + 1);
        //     memcpy(buf, state->input.data+state->input.written, to_consume);
        //     buf[to_consume] = 0;
        //     printf("\nNREAD: %s\n", buf);
        //     zfree(buf);
        // }

        *consumed += to_consume;

        state->input.written += to_consume;

        int decompressed = decompressInto(state, output_buf + tot_decompressed,
                                          output_len - tot_decompressed);
        if (decompressed <= 0)
            break;
        tot_decompressed += decompressed;
    }

    return tot_decompressed;
}

int clientHasPendingCompressionFlush(client *c) {
    compressionState *state = c->compression_state;
    if (!state) return 0;
    if (state->dir != COMPRESS) return 0;

    return state->write_flush_pending || (mstime() - state->last_write >= server.compression_max_latency);
}

int clientHasPendingCompressedData(client *c) {
    compressionState *state = c->compression_state;
    if (!state) return 0;
    if (state->dir != DECOMPRESS) return 0;

    return state->read_flush_pending ||
           state->input.written > state->input.consumed ||
           state->output.written > state->output.consumed;
}

