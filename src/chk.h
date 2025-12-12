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

    uint64_t fp; /* Heap uses different hash than the cuckoo tables */
} chkHeapBucket;

typedef struct chkTopK {
    chkBucket *tables[CHK_NUM_TABLES];
    chkHeapBucket *heap;

    double lut_decay_exp[CHK_LUT_SIZE + 1];
    double lut_min_decay[CHK_LUT_SIZE + 1];
    double lut_decay_prob[CHK_LUT_SIZE + 1];

    double decay;
    double inv_decay;

    counter_t total;

    int k;
    int numbuckets;
} chkTopK;

chkTopK *chkTopKCreate(int k, int numbuckets, double decay);
void chkTopKDestroy(chkTopK *topk);
char *chkTopKUpdate(chkTopK *topk, char *item, int itemlen, counter_t weight);

chkHeapBucket *chkTopKList(chkTopK *topk);
