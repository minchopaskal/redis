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
 * Original implementation based on the paper
 * `HeavyKeeper: An Accurate Algorithm for Finding Top-k Elephant Flows`
 *
 * Modifications on the decay operation is based on the paper
 * `Cuckoo Heavy Keeper and the balancing act of maintaining heavy hitters in
 * stream processing` - section `4.4 Weighted update`.
 *
 */

#include "topk.h"

#include "murmurhash.h"
#include "zmalloc.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define TOPK_HASH(item, itemlen, i) MurmurHash64A(item, itemlen, i)
#define GA 1919

static inline uint32_t topKMax(counter_t a, counter_t b) { return a > b ? a : b; }

static inline char *topKStrndup(const char *s, size_t n) {
    char *ret = zcalloc(n + 1);
    if (ret) {
        memcpy(ret, s, n);
        ret[n] = '\0';
    }
    return ret;
}

void heapifyDown(topKHeapBucket *array, size_t len, size_t start) {
    size_t child = start;

    // check whether larger than children
    if (len < 2 || (len - 2) / 2 < child) {
        return;
    }
    child = 2 * child + 1;
    if ((child + 1) < len && (array[child].count > array[child + 1].count)) {
        ++child;
    }
    if (array[child].count > array[start].count) {
        return;
    }

    // swap while larger than child
    topKHeapBucket top = {0};
    memcpy(&top, &array[start], sizeof(topKHeapBucket));
    do {
        memcpy(&array[start], &array[child], sizeof(topKHeapBucket));
        start = child;

        if ((len - 2) / 2 < child) {
            break;
        }
        child = 2 * child + 1;

        if ((child + 1) < len && (array[child].count > array[child + 1].count)) {
            ++child;
        }
    } while (array[child].count < top.count);
    memcpy(&array[start], &top, sizeof(topKHeapBucket));
}

topK *topKCreate(uint32_t k, uint32_t width, uint32_t depth, double decay) {
    assert(k > 0);
    assert(width > 0);
    assert(depth > 0);
    assert(decay > 0 && decay <= 1);

    if (depth > SIZE_MAX / width || (size_t)depth * width > SIZE_MAX / sizeof(topKBucket)) {
        return NULL;
    }

    topK *topk = (topK *)zcalloc_num(1, sizeof(topK));
    topk->k = k;
    topk->width = width;
    topk->depth = depth;
    topk->invDecay = decay;
    topk->decay = 1./ decay;
    topk->data = ztrycalloc(((size_t)width) * depth * sizeof(topKBucket));
    if (!topk->data) {
        zfree(topk);
        return NULL;
    }

    topk->heap = ztrycalloc(k * sizeof(topKHeapBucket));
    if (!topk->heap) {
        zfree(topk->data);
        zfree(topk);
        return NULL;
    }

    topk->lutDecayPowMinusX[0] = 1;
    topk->lutExpDecayOps[0] = 0;
    topk->lutDecayPowX[0] = 1;

    for (uint32_t i = 1; i < TOPK_DECAY_LOOKUP_TABLE; ++i) {
        topk->lutDecayPowMinusX[i] = pow(topk->invDecay, i);
        topk->lutDecayPowX[i] = pow(topk->decay, i);
        topk->lutExpDecayOps[i] = topk->lutExpDecayOps[i - 1] + topk->lutDecayPowX[i];
    }

    return topk;
}

void topKDestroy(topK *topk) {
    if (!topk) {
        return;
    }

    if (topk->heap) {
        for (uint32_t i = 0; i < topk->k; ++i) {
            if (topk->heap[i].item) {
                zfree(topk->heap[i].item);
            }
        }

        zfree(topk->heap);
        topk->heap = NULL;
    }
    if (topk->data) {
        zfree(topk->data);
        topk->data = NULL;
    }
    zfree(topk);
}

// Complexity O(k + strlen)
static topKHeapBucket *checkExistInHeap(topK *topk, const char *item, size_t itemlen) {
    uint64_t fp = TOPK_HASH(item, itemlen, GA);
    topKHeapBucket *runner = topk->heap;

    for (int32_t i = topk->k - 1; i >= 0; --i)
        if (fp == (runner + i)->fp && itemlen == (runner + i)->itemlen &&
            memcmp((runner + i)->item, item, itemlen) == 0) {
            return runner + i;
        }
    return NULL;
}

static inline double fastPow(double a, double b) {
  // calculate approximation with fraction of the exponent
  int e = (int) b;
  union {
    double d;
    int x[2];
  } u = { a };
  u.x[1] = (int)((b - e) * (u.x[1] - 1072632447) + 1072632447);
  u.x[0] = 0;

  // exponentiation by squaring with the exponent's integer part
  // double r = u.d makes everything much slower, not sure why
  double r = 1.0;
  while (e) {
    if (e & 1) {
      r *= a;
    }
    a *= a;
    e >>= 1;
  }

  return r * u.d;
}

static inline double powDecayMinusX(topK *topk, counter_t cnt) {
    if (cnt < TOPK_DECAY_LOOKUP_TABLE) {
        return topk->lutDecayPowMinusX[cnt];
    }

    /* Using precalculate lookup table to save cpu */
    return fastPow(topk->lutDecayPowMinusX[TOPK_DECAY_LOOKUP_TABLE - 1],
               (double)(cnt / (TOPK_DECAY_LOOKUP_TABLE - 1))) *
           topk->lutDecayPowMinusX[cnt % (TOPK_DECAY_LOOKUP_TABLE - 1)];
}

static inline double powDecayX(topK *topk, counter_t cnt) {
    if (cnt < TOPK_DECAY_LOOKUP_TABLE) {
        return topk->lutDecayPowX[cnt];
    }

    /* Using precalculate lookup table to save cpu */
    return fastPow(topk->lutDecayPowX[TOPK_DECAY_LOOKUP_TABLE - 1],
               (double)(cnt / (TOPK_DECAY_LOOKUP_TABLE - 1))) *
           topk->lutDecayPowX[cnt % (TOPK_DECAY_LOOKUP_TABLE - 1)];
}

static inline double expDecayOps(topK *topk, counter_t cnt, double min_decay_ops) {
    if (cnt < TOPK_DECAY_LOOKUP_TABLE) {
        return topk->lutExpDecayOps[cnt];
    }

    return topk->decay * (min_decay_ops - 1.) / (topk->decay - 1.);
}

static inline double minDecayOps(topK *topk, counter_t cnt) {
    return powDecayX(topk, cnt);
}

/* Simulation of weighted exponential decay of a counter */
counter_t decayCounter(topK *topk, counter_t cnt, counter_t increment,
                       counter_t* new_count)
{
    if (increment == 1) {
        double decay = powDecayMinusX(topk, cnt);
        double chance = rand() / (double)RAND_MAX;
        if (chance < decay) {
            --cnt;
            *new_count = 1;
        }
        return cnt;
    }

    counter_t min_decay_ops = minDecayOps(topk, cnt);
    counter_t exp_decay_ops = expDecayOps(topk, cnt, min_decay_ops);
    if (increment < min_decay_ops) {
        double prob = (double)increment / min_decay_ops;
        double chance = rand() / (double)RAND_MAX;
        if (chance < prob) {
            --cnt;
            /* New count is computed in case cnt has reached 0.
             * Since we simulate multiple decreases here with a formula
             * instead of running it in a loop we approximate how much
             * times we needed to try to decay the cnt via change * increment.
             * Thus what's left is increment - change * increment.
             *
             * Example:
             * min_decay_ops == 100, i.e we need on avg 100 ops to decay
             * the counter. This is actually decay ^ cnt.
             *
             * prob == 90, i.e if increment is 90 we will try to decay
             * the counter 90 times, i.e 90% chance to actually decay it
             * with 1.
             *
             * chance == 50, i.e after 50 tries we actually manage to
             * decay it, thus we are left with increment of 40.
             *
             * Of course this is just an approximation and the paper
             * does not specify how the new count is computed in this
             * case. A better approach might be needed here.
             * */
            *new_count = increment - chance * increment;
        }
    } else if (increment >= exp_decay_ops) {
        cnt = 0;
        /* Same as the case of increment < min_decay_ops but here we
         * know for sure that we will decay the counter to 0.
         * Again we approximate (as per the paper this time) how much
         * of the increment we are left with. */
        *new_count = increment - exp_decay_ops;
    } else {
        /* increment > min_decay_ops[cnt] && increment < exp_decay_ops[*countPtr]
         * Thus we need to find the smallest count X such that
         * exp_decay_ops[X] >= exp_decay_ops[cnt] - increment
         * and that will be our new count. Necessarily 0 < X < cnt.
         * We do a binary search on the space to save ops. */
        counter_t left = 0;
        counter_t right = cnt;
        while (left < right) {
            counter_t mid = left + (right - left) / 2;

            if (expDecayOps(topk, mid, mid < cnt ? 0.0 : minDecayOps(topk, mid)) >= exp_decay_ops - increment) {
                right = mid;
            } else {
                left = mid + 1;
            }
        }
        cnt = left;

        /* We don't need to compute new_count here as cnt couldn't have reached
         * 0 - otherwise we would have been in the case
         * increment >= exp_decay_ops. */
        assert(cnt > 0);
    }

    return cnt;
}

char *topKInsert(topK *topk, const char *item, size_t itemlen, counter_t increment) {
    assert(topk);
    assert(item);
    assert(increment > 0);

    topKBucket *runner;
    counter_t *countPtr;
    counter_t maxCount = 0;
    uint64_t fp = TOPK_HASH(item, itemlen, GA);

    counter_t heapMin = topk->heap->count;

    // get max item count
    for (uint32_t i = 0; i < topk->depth; ++i) {
        uint64_t loc = TOPK_HASH(item, itemlen, i) % topk->width;
        runner = topk->data + i * topk->width + loc;
        countPtr = &runner->count;
        if (*countPtr == 0) {
            runner->fp = fp;
            *countPtr = increment;
            maxCount = topKMax(maxCount, *countPtr);
        } else if (runner->fp == fp) {
            *countPtr += increment;
            maxCount = topKMax(maxCount, *countPtr);
        } else {
            counter_t new_item_count;
            *countPtr = decayCounter(topk, *countPtr, increment, &new_item_count);

            if (*countPtr == 0) {
                runner->fp = fp;
                *countPtr = new_item_count;
                maxCount = topKMax(maxCount, *countPtr);
            }
        }
    }

    /* update heap */
    if (maxCount >= heapMin) {
        topKHeapBucket *itemHeapPtr = checkExistInHeap(topk, item, itemlen);
        if (itemHeapPtr != NULL) {
            itemHeapPtr->count = maxCount; // Not max of the two, as it might have been decayed
            heapifyDown(topk->heap, topk->k, itemHeapPtr - topk->heap);
        } else {
            // zfree(topk->heap[0].item);
            char *expelled = topk->heap[0].item;

            topk->heap[0].count = maxCount;
            topk->heap[0].fp = fp;
            topk->heap[0].item = topKStrndup(item, itemlen);
            topk->heap[0].itemlen = itemlen;
            heapifyDown(topk->heap, topk->k, 0);
            return expelled;
        }
    }
    return NULL;
}

bool topKQuery(topK *topk, const char *item, size_t itemlen) {
    return checkExistInHeap(topk, item, itemlen) != NULL;
}

size_t topKCount(topK *topk, const char *item, size_t itemlen) {
    assert(topk);
    assert(item);

    topKBucket *runner = NULL;
    uint64_t fp = TOPK_HASH(item, itemlen, GA);
    // TODO: The optimization of >heapMin should be revisited for performance
    counter_t heapMin = topk->heap->count;
    topKHeapBucket *heapPtr = checkExistInHeap(topk, item, itemlen);
    counter_t res = 0;

    for (uint32_t i = 0; i < topk->depth; ++i) {
        uint64_t loc = TOPK_HASH(item, itemlen, i) % topk->width;
        runner = topk->data + i * topk->width + loc;
        if (runner->fp == fp && (heapPtr == NULL || runner->count >= heapMin)) {
            res = topKMax(res, runner->count);
        }
    }
    return res;
}

int cmptopKHeapBucket(const void *tmp1, const void *tmp2) {
    const topKHeapBucket *res1 = tmp1;
    const topKHeapBucket *res2 = tmp2;
    return res1->count < res2->count ? 1 : res1->count > res2->count ? -1 : 0;
}

topKHeapBucket *topKList(topK *topk) {
    topKHeapBucket *heapList = zcalloc(topk->k * (sizeof(*heapList)));
    memcpy(heapList, topk->heap, topk->k * sizeof(topKHeapBucket));
    qsort(heapList, topk->k, sizeof(*heapList), cmptopKHeapBucket);
    return heapList;
}
