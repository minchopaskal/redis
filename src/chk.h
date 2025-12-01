/* Implementation of a topK structure using CuckooHeavyKeeper algorithm
 *
 * Implementation is based on the paper "Cuckoo Heavy Keeper and the balancing
 * act of maintaining heavy hitters in stream processing" by Vinh Quang Ngo and
 * Marina Papatriantafilou. Also, the accompanying C++ implementation was used
 * as a reference point: https://github.com/vinhqngo5/Cuckoo_Heavy_Keeper
 * Main changes are addition of a min-heap so we can keep names of the top K
 * elements - idea comes from RedisBloom's TopK structure.
 *
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#pragma once

#include "stdint.h"

#define CHK_LUT_SIZE 256
#define CHK_HEAVY_ENTRIES_PER_BUCKET 2
#define CHK_NUM_TABLES 2

typedef uint64_t counter_t;
typedef uint16_t fingerprint_t;
typedef uint8_t lobby_counter_t;

typedef struct {
    counter_t count;
    fingerprint_t fp;
} chkHeavyEntry;

typedef struct {
    fingerprint_t fp;
    lobby_counter_t count;
} chkLobbyEntry;

typedef struct {
    chkHeavyEntry heavy_entries[CHK_HEAVY_ENTRIES_PER_BUCKET];
    chkLobbyEntry lobby_entry;
    int num_heavy_entries;
} chkBucket;

typedef struct {
    counter_t count;
    char *item;
    int itemlen;

    uint64_t fp; /* Fingerprint used to identify the item. Internal use only */
} chkHeapBucket;

typedef struct chkTopK {
    chkBucket *tables[CHK_NUM_TABLES];
    chkHeapBucket *heap;

    /* Expected number of operations to decay count i to 0 */
    double lut_decay_exp[CHK_LUT_SIZE + 1];

    /* Minimum number of decay operations to decay count i with 1 */
    double lut_min_decay[CHK_LUT_SIZE + 1];

    /* Probability of decaying i with 1. As per paper probability is decay^-i
     * but we actually store (1/decay)^i for faster computation. */
    double lut_decay_prob[CHK_LUT_SIZE + 1];

    /* Decay constant */
    double decay;
    double inv_decay;

    counter_t total;

    int k;
    int numbuckets;
} chkTopK;

chkTopK *chkTopKCreate(int k, int numbuckets, double decay);
void chkTopKDestroy(chkTopK *topk);

/* Update weighted item. If another one was expelled from the topK list -
 * return its name. Caller is responsible for releasing it */
char *chkTopKUpdate(chkTopK *topk, char *item, int itemlen, counter_t weight);

/* Get an ordered by count list of topk->k elements inside the topk object.
 *
 * NOTE, the returned array is a copy of the internal heap stored by `topk`. The
 * caller is responsible for releasing it after use. The elements of the array
 * share their `item` pointers with the internal topk->heap buckets so one must
 * not use it after `topk` is released. */
chkHeapBucket *chkTopKList(chkTopK *topk);

#ifdef REDIS_TEST

int chkTopKTest(int argc, char *argv[], int flags);

#endif /* REDIS_TEST */
