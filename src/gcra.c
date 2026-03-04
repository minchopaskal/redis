/*
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */
#include "server.h"
#include <math.h>

/* GCRA key max_burst requests_per_period period [NUM_REQUESTS count]
 *
 * key: Key related to specific rate limiting case
 * max_burst: Maximum tokens allowed as burst (in addition to sustained rate)
 * requests_per_period: Number of requests allowed per period
 * period: Period in seconds for calculating sustained rate
 * num_requests: Optional, cost of this request (default: 1)
 */
void gcraCommand(client *c) {
    robj *key = c->argv[1];
    long max_burst, requests_per_period;
    long num_requests = 1;
    double period;

    if (c->argc > 7) {
        addReplyErrorArity(c);
        return;
    }

    if (getPositiveLongFromObjectOrReply(c, c->argv[2], &max_burst, NULL) != C_OK) {
        return;
    }
    if (likely(max_burst < LONG_MAX)) max_burst += 1;

    if (getRangeLongFromObjectOrReply(c, c->argv[3], 1, LONG_MAX, &requests_per_period, NULL) != C_OK) {
        return;
    }

    if (getDoubleFromObjectOrReply(c, c->argv[4], &period, NULL) != C_OK) {
        return;
    }
    if (period <= 0) {
        addReplyError(c, "period must be > 0");
        return;
    }

    if (c->argc >= 6) {
        if (strcasecmp("NUM_REQUESTS", c->argv[5]->ptr)) {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
        if (c->argc == 6) {
            addReplyError(c, "Missing NUM_REQUESTS value");
            return;
        }
        if (getRangeLongFromObjectOrReply(c, c->argv[6], 1, LONG_MAX, &num_requests, NULL) != C_OK) {
            return;
        }
    }

    ustime_t now = commandTimeSnapshot() * 1000;

    long long tat_us, new_tat_us;
    dictEntryLink link;
    kvobj *kv = lookupKeyWriteWithLink(c->db, key, &link);
    if (checkType(c, kv, OBJ_STRING)) {
        return;
    }
    if (kv != NULL) {
        if (getLongLongFromObject(kv, &tat_us) != C_OK) {
            addReplyError(c, "Invalid GCRA key");
            return;
        }
    } else {
        tat_us = now;
    }

    /* Variables used in the reply */
    int limited;
    long long remaining = 0, reset_after_s = 0, retry_after_s = -1;

    /* microsecond accuracy */
    double period_us = period * 1000000.;
    long long emission_interval_us = (long long)(period_us / requests_per_period);
    if (unlikely(emission_interval_us == 0)) emission_interval_us = 1;
    long long increment_us = emission_interval_us * num_requests;
    long long variance_us = emission_interval_us * max_burst;
    long long ttl_us;

    if (now > tat_us) {
        new_tat_us = now + increment_us;
    } else {
        new_tat_us = tat_us + increment_us;
    }

    long long allow_at = new_tat_us - variance_us;
    long long diff_us = now - allow_at;
    if (diff_us < 0) {
        limited = 1;
        if (increment_us < variance_us) {
            retry_after_s = ceil((-diff_us) / 1000000.);
        }
        ttl_us = tat_us - now;
    } else {
        limited = 0;
        ttl_us = new_tat_us - now;
        robj *tatobj = createStringObjectFromLongLong(new_tat_us);
        if (kv) {
            setKeyByLink(c, c->db, key, &tatobj, SETKEY_ALREADY_EXIST, &link);
        } else {
            setKeyByLink(c, c->db, key, &tatobj, SETKEY_DOESNT_EXIST, &link);
        }
        long long when = new_tat_us / 1000;
        kv = setExpireByLink(c, c->db, key->ptr, when, link);
    }

    long long next_us = variance_us - ttl_us;
    remaining = 0;
    if (next_us > -emission_interval_us) {
        remaining = next_us / emission_interval_us;
    }
    reset_after_s = ceil(ttl_us / 1000000.);

    addReplyArrayLen(c, 5);
    addReply(c, limited ? shared.cone : shared.czero);
    addReplyLongLong(c, max_burst);
    addReplyLongLong(c, remaining);
    addReplyLongLong(c, retry_after_s);
    addReplyLongLong(c, reset_after_s);
}
