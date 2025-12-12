#include "chk.h"

#include "murmurhash.h"
#include "zmalloc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOBBY_PROMOTION_THRESHOLD 16

#ifndef static_assert
#define static_assert(expr, lit) extern char __static_assert_failure[(expr) ? 1:-1]
#endif

static_assert(LOBBY_PROMOTION_THRESHOLD < CHK_LUT_SIZE,
              "Lobby promotion threshold should be less then the LUT size to "
              "ensure constant operations during decayCounter!");

#define MAX_KICKS 16
#define HEAVY_RATIO 0.008
#define HEAP_SEED 1919

typedef struct {
    int idx[CHK_NUM_TABLES];
    uint16_t fp;
} fpAndIdx;

static inline counter_t chkMax(counter_t a, counter_t b) {
    return a > b ? a : b;
}

static inline char *chkStrndup(const char *s, size_t n) {
    char *ret = zcalloc(n + 1);
    if (ret) {
        memcpy(ret, s, n);
        ret[n] = '\0';
    }
    return ret;
}

/* Heap operations */
static chkHeapBucket *chkCheckExistInHeap(chkTopK *topk, const char *item, int itemlen,
                                       uint64_t fp) {
    chkHeapBucket *runner = topk->heap;

    for (int32_t i = topk->k - 1; i >= 0; --i)
        if (fp == (runner + i)->fp && itemlen == (runner + i)->itemlen &&
            memcmp((runner + i)->item, item, itemlen) == 0) {
            return runner + i;
        }
    return NULL;
}

void chkHeapifyDown(chkHeapBucket *array, size_t len, size_t start) {
    size_t child = start;

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

    chkHeapBucket top = {0};
    memcpy(&top, &array[start], sizeof(chkHeapBucket));
    do {
        memcpy(&array[start], &array[child], sizeof(chkHeapBucket));
        start = child;

        if ((len - 2) / 2 < child) {
            break;
        }
        child = 2 * child + 1;

        if ((child + 1) < len && (array[child].count > array[child + 1].count)) {
            ++child;
        }
    } while (array[child].count < top.count);
    memcpy(&array[start], &top, sizeof(chkHeapBucket));
}

/* chkTopK operations */

chkTopK *chkTopKCreate(int k, int numbuckets, double decay) {
    /* Number of buckets need to be a power of 2 for better performance */
    if ((numbuckets & (numbuckets - 1)) != 0) {
        return NULL;
    }

    chkTopK *topk = ztrymalloc(sizeof(chkTopK));
    if (!topk) return NULL;

    for (int i = 0; i < CHK_NUM_TABLES; ++i) {
        topk->tables[i] = ztrycalloc(sizeof(chkBucket) * numbuckets);

        if (topk->tables[i] == NULL) {
            for (int j = 0; j < i; ++j)
                zfree(topk->tables[j]);

            zfree(topk);
            return NULL;
        }
    }

    topk->heap = ztrycalloc(sizeof(chkHeapBucket) * k);
    if (topk->heap == NULL) {
        for (int i = 0; i < CHK_NUM_TABLES; ++i)
            zfree(topk->tables[i]);

        zfree(topk);
        return NULL;
    }

    topk->k = k;
    topk->numbuckets = numbuckets;
    topk->decay = decay;
    topk->inv_decay = 1. / decay;
 
    topk->lut_decay_exp[0] = 0;
    topk->lut_min_decay[0] = 0;
    topk->lut_decay_prob[0] = 0;
    for (int i = 1; i < CHK_LUT_SIZE + 1; ++i) {
        topk->lut_decay_exp[i] = topk->lut_decay_exp[i - 1] + pow(topk->decay, i - 1);
        topk->lut_min_decay[i] = topk->lut_decay_exp[i] - topk->lut_decay_exp[i - 1];

        topk->lut_decay_prob[i] = pow(topk->inv_decay, i);
    }

    return topk;
}

void chkTopKDestroy(chkTopK *topk) {
    for (int i = 0; i < CHK_NUM_TABLES; ++i)
        zfree(topk->tables[i]);
    zfree(topk->heap);
    zfree(topk);
}

fpAndIdx generateItemFpAndIdxs(chkTopK *topk, char *item, int itemlen) {
    uint64_t hash = MurmurHash64A(item, itemlen, 0);

    fpAndIdx res;
    res.fp = (hash & 0xFFFF); /* Only use 16 bits for fingerprint */
    res.idx[0] = (hash >> 32) % topk->numbuckets;
    for (int i = 1; i < CHK_NUM_TABLES; ++i) {
        res.idx[i] = (res.idx[i - 1] ^ (0x5bd1e995 * res.fp)) % topk->numbuckets;
    }

    return res;
}

typedef struct {
    int table_idx;
    int pos;
} checkEntryRes;

checkEntryRes checkHeavyEntries(chkTopK *topk, fpAndIdx item, counter_t weight) {
    int empty_table_idx = -1;
    int empty_pos = -1;

    for (int i = 0; i < CHK_NUM_TABLES; ++i) {
        int idx = item.idx[i];

        chkBucket *bucket = &topk->tables[i][idx];
        for (int j = 0; j < CHK_HEAVY_ENTRIES_PER_BUCKET; ++j) {
            chkHeavyEntry *e = &bucket->heavy_entries[j];
            if (e->fp == item.fp && e->count > 0) {
                e->count += weight;

                checkEntryRes res = { i, j };
                return res;
            } else if (empty_table_idx == -1) {
                empty_table_idx = i;
                empty_pos = j;
            }
        }
    }

    if (empty_table_idx == -1) {
        checkEntryRes res = { -1, -1 };
        return res;
    }

    /* If there is an empty slot in the heavy entries just put the item there
     * instead of going through the lobby first (optimization as per the paper) */
    int idx = item.idx[empty_table_idx];
    chkHeavyEntry *e = &topk->tables[empty_table_idx][idx].heavy_entries[empty_pos];
    e->fp = item.fp;
    e->count = weight;

    checkEntryRes res = {empty_table_idx, empty_pos};
    return res;
}

int isHeavyHitter(chkTopK *topk, counter_t cnt) {
    return cnt >= (topk->total * HEAVY_RATIO);
}

void kickout(chkTopK *topk, chkHeavyEntry entry, fpAndIdx item, int table_idx) {
    for (int i = 0; i < MAX_KICKS; ++i) {
        /* Do not try to swap with any entries if we don't reach the heavy
         * hitter threshold */
        if (!isHeavyHitter(topk, entry.count)) return;

        /* Find the heavy entry in the alt bucket in the other table with
         * minimum count. If there is empty entry there just occupy it, else
         * recursively kick the minimal one out. */
        int other_table_idx = 1 - table_idx;
        int idx = item.idx[other_table_idx];

        chkBucket *bucket = &topk->tables[other_table_idx][idx];
        counter_t min = (counter_t)-1;
        int min_pos = -1;
        for (int j = 0; j < CHK_HEAVY_ENTRIES_PER_BUCKET; ++j) {
            chkHeavyEntry *e = &bucket->heavy_entries[j];
            if (e->count == 0) {
                *e = entry;
                return;
            }
            if (e->count < min) {
                min = e->count;
                min_pos = j;
            }
        }

        chkHeavyEntry old_entry = bucket->heavy_entries[min_pos];
        bucket->heavy_entries[min_pos] = entry;
        entry = old_entry;
    }
}

int tryPromoteAndKickout(chkTopK *topk, fpAndIdx item, counter_t new_count, int table_idx) {
    int idx = item.idx[table_idx];
    chkBucket *bucket = &topk->tables[table_idx][idx];
    counter_t min = (counter_t)-1;
    int min_idx = -1;
    for (int i = 0; i < CHK_HEAVY_ENTRIES_PER_BUCKET; ++i) {
        if (bucket->heavy_entries[i].count == 0) {
            bucket->heavy_entries[i].fp = item.fp;
            bucket->heavy_entries[i].count = new_count;
            return i;
        }
        if (bucket->heavy_entries[i].count < min) {
            min = bucket->heavy_entries[i].count;
            min_idx = i;
        }
    }

    /* If the heavy entry that is going to be kicked out has a counter lower
     * than the lobby's one we always kick it out */
    if (min > new_count) {
        double prob = (new_count - LOBBY_PROMOTION_THRESHOLD) /
                      (double)(min - LOBBY_PROMOTION_THRESHOLD);

        if ((rand() / (double)RAND_MAX) >= prob) return -1;
    }

    chkHeavyEntry to_kickout = bucket->heavy_entries[min_idx];
    bucket->heavy_entries[min_idx].fp =  bucket->lobby_entry.fp;

    bucket->lobby_entry.count = 0;
    bucket->lobby_entry.fp = 0;

    kickout(topk, to_kickout, item, table_idx);

    return min_idx;
}

checkEntryRes checkLobbyEntries(chkTopK *topk, fpAndIdx item, counter_t weight) {
    for (int i = 0; i < CHK_NUM_TABLES; ++i) {
        int idx = item.idx[i];

        chkBucket *bucket = &topk->tables[i][idx];
        chkLobbyEntry *e = &bucket->lobby_entry;

        /* No match or empty lobby entry */
        if (e->fp != item.fp || e->count == 0) continue;

        /* If we don't cross the threshold just update the counter */
        uint64_t new_count = (uint64_t)e->count + weight;
        if (new_count < LOBBY_PROMOTION_THRESHOLD) {
            e->count = (uint16_t)new_count;

            checkEntryRes res = { i, -1 };
            return res;
        }

        /* Try to promote the entry to heavy entry if we crossed the threshold.
         * Else just set the counter to the value of the threshold */
        int kickout_pos = tryPromoteAndKickout(topk, item, new_count, i);
        if (kickout_pos != -1) {
            checkEntryRes res = {i, kickout_pos};
            return res;
        }

        e->count = LOBBY_PROMOTION_THRESHOLD;
        checkEntryRes res = { i, -1 };
        return res;
    }

    checkEntryRes res = { -1, -1 };
    return res;
}

double getDecayProb(chkTopK *topk, counter_t cnt) {
    if (cnt < CHK_LUT_SIZE) {
        return topk->lut_decay_prob[cnt];
    }

    return pow(topk->lut_decay_prob[CHK_LUT_SIZE],
               ((double)cnt / (CHK_LUT_SIZE))) *
           topk->lut_decay_prob[cnt % (CHK_LUT_SIZE)];
}

double getExpDecayCount(chkTopK *topk, counter_t cnt) {
    if (cnt < CHK_LUT_SIZE) {
        return topk->lut_decay_exp[cnt];
    }

    return pow(topk->lut_decay_exp[CHK_LUT_SIZE],
               ((double)cnt / (CHK_LUT_SIZE))) *
           topk->lut_decay_exp[cnt % (CHK_LUT_SIZE)];
}

double getMinDecayCount(chkTopK *topk, counter_t cnt) {
    if (cnt < CHK_LUT_SIZE) {
        return topk->lut_min_decay[cnt];
    }

    return pow(topk->lut_min_decay[CHK_LUT_SIZE],
               ((double)cnt / (CHK_LUT_SIZE))) *
           topk->lut_min_decay[cnt % (CHK_LUT_SIZE)];
}

lobby_counter_t chkDecayCounter(chkTopK *topk, lobby_counter_t cnt, counter_t weight) {
    if (weight == 0) return cnt;

    /* Unweighted update - just decay with probability decay ^ -cnt */
    if (weight == 1) {
        double prob = getDecayProb(topk, (counter_t)cnt);
        if ((rand() / (double)RAND_MAX) < prob) {
            return cnt - 1;
        }
        return cnt;
    }

    /* For weighted updates we simulate multiple unweighted ones */

    /* Weight is smaller than the minimum amount of decay steps required to
     * decay the counter with probability of 100% so again we roll the dice */
    double min_decay = getMinDecayCount(topk, cnt);
    if (weight < (counter_t)min_decay) {
        double prob = weight / min_decay;
        if ((rand() / (double)RAND_MAX) < prob) {
            return cnt - 1;
        }
        return cnt;
    }

    /* Weight is more than the expected amount of decay steps to decay the
     * counter to 0. */
    double exp_decays = getExpDecayCount(topk, cnt);
    if (weight >= (counter_t)exp_decays)
        return 0;

    /* Weight is large enough to decay the counter to cnt - X where 0 < X < cnt.
     * We binary search for such count C that
     * lut_decay_exp[C] >= lut_decay_exp[cnt] - weight.
     *
     * Note that since cnt is a lobby counter it will necesarily be less or equal
     * than LOBBY_PROMOTION_THRESHOLD, so although we binary search this is a
     * constant operation */
    int left = 0;
    int right = cnt;
    while (left < right) {
        int mid = left + (right - left) / 2;

        if (topk->lut_decay_exp[mid] + weight >= topk->lut_decay_exp[cnt]) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }

    return left;
}

char *chkTopKUpdate(chkTopK *topk, char *item, int itemlen, counter_t weight) {
    topk->total += weight;

    fpAndIdx itemFpIdx = generateItemFpAndIdxs(topk, item, itemlen);

    checkEntryRes res = checkHeavyEntries(topk, itemFpIdx, weight);
    if (res.table_idx != -1) {
        goto update_heap;
    }

    res = checkLobbyEntries(topk, itemFpIdx, weight);
    if (res.table_idx != -1) {
        goto update_heap;
    }

    /* Check for empty lobby entries */
    for (int i = 0; i < CHK_NUM_TABLES; ++i) {
        int idx = itemFpIdx.idx[i];
        chkBucket *bucket = &topk->tables[i][idx];
        if (bucket->lobby_entry.count == 0) {
            bucket->lobby_entry.fp = itemFpIdx.fp;

            res.table_idx = i;
            res.pos = -1;

            if (weight < LOBBY_PROMOTION_THRESHOLD) {
                bucket->lobby_entry.count = weight;
            } else {
                int kickout_pos = tryPromoteAndKickout(topk, itemFpIdx, weight, i);
                if (kickout_pos != -1) {
                    res.pos = kickout_pos;
                } else {
                    bucket->lobby_entry.count = LOBBY_PROMOTION_THRESHOLD;
                }
            }

            goto update_heap;
        }
    }
 
    /* If no empty lobby entries choose a table deterministically and decay its
     * lobby counter and update */
    int table_idx = itemFpIdx.fp & 1;
    int idx = itemFpIdx.idx[table_idx];

    chkLobbyEntry *e = &topk->tables[table_idx][idx].lobby_entry;
    lobby_counter_t new_count = chkDecayCounter(topk, e->count, weight);

    if (new_count == 0) {
        e->fp = itemFpIdx.fp;
        e->count = chkMax(1, weight - getExpDecayCount(topk, e->count));
    } else {
        e->count = new_count;
    }

    if (e->count >= LOBBY_PROMOTION_THRESHOLD) {
        int kickout_pos = tryPromoteAndKickout(topk, itemFpIdx, e->count, table_idx);
        if (kickout_pos != -1) {
            res.table_idx = table_idx;
            res.pos = kickout_pos;
        }
    }

update_heap:
    if (res.table_idx == -1 || res.pos == -1)
        return NULL;

    table_idx = res.table_idx;
    idx = itemFpIdx.idx[table_idx];

    counter_t heap_min = topk->heap[0].count;
    chkHeavyEntry *entry = &topk->tables[table_idx][idx].heavy_entries[res.pos];
 
    if (entry->count < heap_min)
        return NULL;

    uint64_t fp = MurmurHash64A(item, itemlen, HEAP_SEED);
    chkHeapBucket *itemHeapPtr = chkCheckExistInHeap(topk, item, itemlen, fp);
    if (itemHeapPtr != NULL) {
        itemHeapPtr->count = entry->count;
        chkHeapifyDown(topk->heap, topk->k, itemHeapPtr - topk->heap);
    } else {
        char *expelled = topk->heap[0].item;

        topk->heap[0].count = entry->count;
        topk->heap[0].fp = fp;
        topk->heap[0].item = chkStrndup(item, itemlen);
        topk->heap[0].itemlen = itemlen;
        chkHeapifyDown(topk->heap, topk->k, 0);
        return expelled;
    }

    return NULL;
}

int cmpchkHeapBucket(const void *tmp1, const void *tmp2) {
    const chkHeapBucket *res1 = tmp1;
    const chkHeapBucket *res2 = tmp2;
    return res1->count < res2->count ? 1 : res1->count > res2->count ? -1 : 0;
}

chkHeapBucket *chkTopKList(chkTopK *topk) {
    chkHeapBucket *list = ztrycalloc(sizeof(chkHeapBucket) * topk->k);
    if (list == NULL) return NULL;

    memcpy(list, topk->heap, sizeof(chkHeapBucket) * topk->k);
    qsort(list, topk->k, sizeof(*list), cmpchkHeapBucket);
    return list;
}
