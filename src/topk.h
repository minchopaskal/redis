/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Implementation by Ariel Shtul
 *
 * Note that this file is slight modification of RedisBloom's TopK structure
 *
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#define TOPK_DECAY_LOOKUP_TABLE 256

typedef uint64_t counter_t;

typedef struct topKHeapBucket {
    uint64_t fp;
    counter_t count;
    char *item;
    uint32_t itemlen;
} topKHeapBucket;

typedef struct topKBucket {
    uint64_t fp; //  fingerprint
    counter_t count;
} topKBucket;

typedef struct topK {
    uint32_t k;
    uint32_t width;
    uint32_t depth;
    double decay;
    double invDecay;
    topKBucket *data;
    topKHeapBucket *heap;

    /* Probability to decay counter with 1: decay ^ -i */
    double lutDecayPowMinusX[TOPK_DECAY_LOOKUP_TABLE];

    /* Expected number of ops to decay `i` to `i-1` is decay ^ i, since the
     * probability is decay ^ -i. */
    double lutDecayPowX[TOPK_DECAY_LOOKUP_TABLE];

    /* Expected number of ops to decay counter k to 0. */
    double lutExpDecayOps[TOPK_DECAY_LOOKUP_TABLE];
} topK;

/* Returns a new Top-K DS which will keep to 'k' heavyhitter, using
 * 'depth' arrays of 'width' counters at 'decay' rate.
 * Complexity - O(1) */
topK *topKCreate(uint32_t k, uint32_t width, uint32_t depth, double decay);

/* Releases resources of a Top-K DS.
 * Complexity - O(k) */
void topKDestroy(topK *topk);

/* Inserts an 'item' with length 'itemlen' into 'topk' DS.
 * Return value is NULL if no change to Top-K list occurred else,
 * it returns the item expelled from list. If returned pointer
 * is not NULL, it should be free()d.
 * Complexity - O(k) */
char *topKInsert(topK *topk, const char *item, size_t itemlen, counter_t increment);

/* Checks whether an 'item' is in Top-K list of 'topk'.
 * Complexity - O(k) */
bool topKQuery(topK *topk, const char *item, size_t itemlen);

/* Returns count for an 'item' in 'topk' DS.
 * This number can be significantly lower than real count.
 * Complexity - O(k) */
size_t topKCount(topK *topk, const char *item, size_t itemlen);

/*  Returns full 'heapList' of items in 'topk' DS. User is responsible for
 *  freeing the list. */
topKHeapBucket *topKList(topK *topk);
