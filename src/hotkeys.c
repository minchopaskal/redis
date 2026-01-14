/* Hotkey tracking related functionality
 *
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "server.h"
#include "chk.h"
#include "cluster.h"

static inline int nearestNextPowerOf2(unsigned int count) {
    return count == 0 ? 1 : (1 << (32 - __builtin_clz(count)));
}

hotkeyStats *hotkeyStatsInit(int count, int duration, int sample_ratio,
                             int *slots, int slots_count, uint64_t tracked_metrics)
{
    hotkeyStats *hotkeys = zcalloc(sizeof(hotkeyStats));

    /* We track count * 10 keys for better accuracy. Numbuckets is roughly 10
     * times the elements we track (actually num_buckets == 7-8 * count is
     * enough) again for better accuracy. Note the CHK implementation uses a
     * power of 2 numbuckets for better cache locality. */
    if (tracked_metrics & HOTKEYS_TRACK_CPU) {
        hotkeys->cpu = chkTopKCreate(count * 10,
                                     nearestNextPowerOf2((unsigned)count * 100),
                                     1.08);
        if (!hotkeys->cpu) {
            hotkeyStatsRelease(hotkeys);
            return NULL;
        }
    }

    if (tracked_metrics & HOTKEYS_TRACK_NET) {
        hotkeys->net = chkTopKCreate(count * 10,
                                     nearestNextPowerOf2((unsigned)count * 100),
                                     1.08);
        if (!hotkeys->net) {
            hotkeyStatsRelease(hotkeys);
            return NULL;
        }
    }

    /* At least one metric must be enabled */
    if (tracked_metrics == 0) {
        hotkeyStatsRelease(hotkeys);
        return NULL;
    }

    hotkeys->tracked_metrics = tracked_metrics;
    hotkeys->k = count;
    hotkeys->duration = duration;
    hotkeys->sample_ratio = sample_ratio;
    hotkeys->slots = slots;
    hotkeys->numslots = slots_count;
    hotkeys->active = 1;

    hotkeys->keys_result.size = MAX_KEYS_BUFFER;

    /* Store initial rusage for CPU time tracking */
    getrusage(RUSAGE_SELF, &hotkeys->rusage_start);

    hotkeys->start = mstime();

    return hotkeys;
}

void hotkeyStatsRelease(hotkeyStats *hotkeys) {
    if (!hotkeys) return;
    if (hotkeys->cpu) chkTopKDestroy(hotkeys->cpu);
    if (hotkeys->net) chkTopKDestroy(hotkeys->net);
    zfree(hotkeys->slots);
    getKeysFreeResult(&hotkeys->keys_result);

    zfree(hotkeys);
}

/* Helper function for hotkey tracking to check if a slot is in the selected
 * slots list. If numslots is 0 then all slots are selected. */
static inline int isSlotSelected(hotkeyStats *hotkeys, int slot) {
    if (hotkeys->numslots == 0) return 1;
    for (int i = 0; i < hotkeys->numslots; i++) {
        if (hotkeys->slots[i] == slot) return 1;
    }
    return 0;
}

void hotkeyStatsPreCurrentCmd(hotkeyStats *hotkeys, client *c) {
    if (!hotkeys || !hotkeys->active) return;

    robj **argv = c->original_argv ? c->original_argv : c->argv;
    int argc = c->original_argv ? c->original_argc : c->argc;

    hotkeys->keys_result.numkeys = 0;
    hotkeys->keys_result.size = MAX_KEYS_BUFFER;
    if (getKeysFromCommandWithSpecs(c->realcmd, argv, argc, GET_KEYSPEC_DEFAULT,
                                    &hotkeys->keys_result) == 0)
    {
        return;
    }

    /* Check if command is sampled */
    hotkeys->is_sampled = 1;
    if (hotkeys->sample_ratio > 1 &&
        (double)rand() / RAND_MAX >= 1.0 / hotkeys->sample_ratio)
    {
        hotkeys->is_sampled = 0;
    }

    hotkeys->is_in_selected_slots = isSlotSelected(hotkeys, c->slot);

    hotkeys->current_client = c;
}

void hotkeyStatsUpdateCurrentCmd(hotkeyStats *hotkeys, hotkeyMetrics metrics) {
    if (!hotkeys || !hotkeys->active) return;
    if (hotkeys->keys_result.numkeys == 0) return;
    serverAssert(hotkeys->current_client);

    int numkeys = hotkeys->keys_result.numkeys;
    uint64_t duration_per_key = metrics.cpu_time_usec / numkeys;
    uint64_t total_bytes = metrics.net_bytes;
    uint64_t bytes_per_key = total_bytes / numkeys;

    /* Update statistics counters */
    hotkeys->time_all_commands_all_slots += metrics.cpu_time_usec;
    hotkeys->net_bytes_all_commands_all_slots += total_bytes;

    if (hotkeys->is_in_selected_slots) {
        hotkeys->time_all_commands_selected_slots += metrics.cpu_time_usec;
        hotkeys->net_bytes_all_commands_selected_slots += total_bytes;

        if (hotkeys->is_sampled && hotkeys->sample_ratio > 1) {
            hotkeys->time_sampled_commands_selected_slots += metrics.cpu_time_usec;
            hotkeys->net_bytes_sampled_commands_selected_slots += total_bytes;
        }
    }

    /* Only add keys to topK structure if command was sampled and is in selected
     * slots. */
    if (!hotkeys->is_sampled || !hotkeys->is_in_selected_slots) {
        return;
    }

    long long start_time = mstime();

    /* Keys we've cached in the keys_result only track positions in the client's
     * argv array so we must fetch it. */
    client *c = hotkeys->current_client;
    robj **argv = c->original_argv ? c->original_argv : c->argv;

    /* Add all keys to topK structure */
    int len;
    for (int i = 0; i < numkeys; ++i) {
        int pos = hotkeys->keys_result.keys[i].pos;

        if (hotkeys->tracked_metrics & HOTKEYS_TRACK_CPU) {
            char *ret = chkTopKUpdate(hotkeys->cpu, argv[pos]->ptr,
                                      sdslen(argv[pos]->ptr),
                                      duration_per_key, &len);
            if (ret) zfree(ret);
        }

        if (hotkeys->tracked_metrics & HOTKEYS_TRACK_NET) {
            char *ret = chkTopKUpdate(hotkeys->net, argv[pos]->ptr,
                                      sdslen(argv[pos]->ptr),
                                      bytes_per_key, &len);
            if (ret) zfree(ret);
        }
    }

    /* Track CPU time spent updating the topk structures. */
    long long end_time = mstime();
    hotkeys->cpu_time += (end_time - start_time);
}

void hotkeyStatsPostCurrentCmd(hotkeyStats *hotkeys) {
    if (!hotkeys || !hotkeys->active) return;

    getKeysFreeResult(&hotkeys->keys_result);
    memset(&hotkeys->keys_result, 0, sizeof(getKeysResult));
    hotkeys->keys_result.size = MAX_KEYS_BUFFER;

    hotkeys->current_client = NULL;
    hotkeys->is_sampled = 0;
    hotkeys->is_in_selected_slots = 0;
}

/* HOTKEYS command implementation
 * 
 * HOTKEYS START
 *         <METRICS count [CPU] [NET]>
 *         [COUNT k]
 *         [DURATION duration]
 *         [SAMPLE ratio]
 *         [SLOTS count slot…]
 * HOTKEYS STOP
 * HOTKEYS RESET
 * HOTKEYS GET
 */
void hotkeysCommand(client *c) {
    if (c->argc < 2) {
        addReplyError(c, "HOTKEYS subcommand required");
        return;
    }

    char *sub = c->argv[1]->ptr;

    if (!strcasecmp(sub, "START")) {
        /* HOTKEYS START
         *         <METRICS count [CPU] [NET]>
         *         [COUNT k]
         *         [DURATION seconds]
         *         [SAMPLE ratio]
         *         [SLOTS count slot…] */
        /* Return error if a session is already started */
        if (server.hotkeys && server.hotkeys->active) {
            addReplyError(c, "hotkey tracking session already in progress");
            return;
        }

        /* METRICS is required and must be the first argument */
        if (c->argc < 4 || strcasecmp(c->argv[2]->ptr, "METRICS")) {
            addReplyError(c, "METRICS parameter is required");
            return;
        }

        long metrics_count;
        if (getRangeLongFromObjectOrReply(c, c->argv[3], 1, HOTKEYS_METRICS_COUNT,
                &metrics_count, "METRICS count must be positive") != C_OK)
        {
            return;
        }

        uint64_t tracked_metrics = 0;

        int j = 4;

        /* Parse CPU and NET tokens */
        int metrics_parsed = 0;
        while (j < c->argc && metrics_parsed < metrics_count) {
            if (!strcasecmp(c->argv[j]->ptr, "CPU")) {
                if (tracked_metrics & HOTKEYS_TRACK_CPU) {
                    addReplyError(c, "METRICS CPU defined more than once!");
                    return;
                }
                tracked_metrics |= HOTKEYS_TRACK_CPU;
                metrics_parsed++;
                j++;
            } else if (!strcasecmp(c->argv[j]->ptr, "NET")) {
                if (tracked_metrics & HOTKEYS_TRACK_NET) {
                    addReplyError(c, "METRICS NET defined more than once!");
                    return;
                }
                tracked_metrics |= HOTKEYS_TRACK_NET;
                metrics_parsed++;
                j++;
            } else {
                break;
            }
        }

        if (metrics_parsed != metrics_count) {
            addReplyError(c, "METRICS count does not match number of metric types provided");
            return;
        }

        int count = 10;  /* default */
        long duration = 0;  /* default: no auto-stop */
        int sample_ratio = 1;  /* default: track every key */
        int slots_count = 0;
        int *slots = NULL;
        while (j < c->argc) {
            int moreargs = (c->argc-1) - j;
            if (moreargs && !strcasecmp(c->argv[j]->ptr, "COUNT")) {
                long count_val;
                if (getRangeLongFromObjectOrReply(c, c->argv[j+1], 1, 64,
                        &count_val, "COUNT must be between 1 and 64") != C_OK)
                {
                    zfree(slots);
                    return;
                }
                count = (int)count_val;
                j += 2;
            } else if (moreargs && !strcasecmp(c->argv[j]->ptr, "DURATION")) {
                /* Arbitrary 1 million seconds limit, so we don't overflow the
                 * duration member which is kept in milliseconds */
                if (getRangeLongFromObjectOrReply(c, c->argv[j+1], 1, 1000000,
                        &duration, "DURATION be between 1 and 1000000") != C_OK)
                {
                    zfree(slots);
                    return;
                }
                duration *= 1000;
                j += 2;
            } else if (moreargs && !strcasecmp(c->argv[j]->ptr, "SAMPLE")) {
                long ratio_val;
                if (getRangeLongFromObjectOrReply(c, c->argv[j+1], 1, INT_MAX,
                        &ratio_val, "SAMPLE ratio must be positive") != C_OK)
                {
                    zfree(slots);
                    return;
                }
                sample_ratio = (int)ratio_val;
                j += 2;
            } else if (moreargs && !strcasecmp(c->argv[j]->ptr, "SLOTS")) {
                if (slots) {
                    addReplyError(c, "SLOTS parameter already specified");
                    zfree(slots);
                    return;
                }
                long slots_count_val;
                char msg[64];
                snprintf(msg, 64, "SLOTS count must be between 1 and %d",
                         CLUSTER_SLOTS);
                if (getRangeLongFromObjectOrReply(c, c->argv[j+1], 1,
                        CLUSTER_SLOTS, &slots_count_val, msg) != C_OK)
                {
                    return;
                }
                slots_count = (int)slots_count_val;

                /* Parse slot numbers */
                if (j + 1 + slots_count >= c->argc) {
                    addReplyError(c, "not enough slot numbers provided");
                    return;
                }
                slots = zmalloc(sizeof(int) * slots_count);
                for (int i = 0; i < slots_count; i++) {
                    long slot_val;
                    if ((slot_val = getSlotOrReply(c, c->argv[j+2+i])) == -1) {
                        zfree(slots);
                        return;
                    }
                    /* Check for duplicate slot indices */
                    for (int j = 0; j < i; ++j) {
                        if (slots[j] == slot_val) {
                            addReplyError(c, "duplicate slot number");
                            zfree(slots);
                            return;
                        }
                    }

                    slots[i] = (int)slot_val;
                }
                j += 2 + slots_count;
            } else {
                addReplyError(c, "syntax error");
                if (slots) zfree(slots);
                return;
            }
        }

        hotkeyStats *hotkeys = hotkeyStatsInit(count, duration, sample_ratio,
                                               slots, slots_count, tracked_metrics);
 
        if (!hotkeys || !hotkeys->active) {
            if (!hotkeys) zfree(slots);
            /* If hotkeys was initialized it will have taken ownership of slots
             * so they will be released insize hotkeyStatsRelease */
            hotkeyStatsRelease(hotkeys);
            addReplyError(c, "hotkey tracking could not be started!");
            return;
        }

        hotkeyStatsRelease(server.hotkeys);
        server.hotkeys = hotkeys;

        addReply(c, shared.ok);

    } else if (!strcasecmp(sub, "STOP")) {
        /* HOTKEYS STOP */
        if (c->argc != 2) {
            addReplyError(c, "wrong number of arguments for 'hotkeys|stop' command");
            return;
        }

        if (!server.hotkeys || !server.hotkeys->active) {
            addReplyNull(c);
            return;
        }

        if (server.hotkeys->active) {
            server.hotkeys->active = 0;
            server.hotkeys->duration = mstime() - server.hotkeys->start;
        }

        addReply(c, shared.ok);

    } else if (!strcasecmp(sub, "GET")) {
        /* HOTKEYS GET */
        if (c->argc != 2) {
            addReplyError(c, "wrong number of arguments for 'hotkeys|get' command");
            return;
        }

        /* If no tracking is started, return (nil) */
        if (!server.hotkeys) {
            addReplyNull(c);
            return;
        }

        if (!server.hotkeys->active && !server.hotkeys->tracked_metrics) {
            addReplyNull(c);
            return;
        }

        /* Calculate duration */
        int duration = 0;
        if (!server.hotkeys->active) {
            duration = server.hotkeys->duration;
        } else {
            duration = mstime() - server.hotkeys->start;
        }

        /* Get total CPU time using rusage (RUSAGE_SELF) -
         * only if CPU tracking is enabled */
        uint64_t total_cpu_user_msec = 0;
        uint64_t total_cpu_sys_msec = 0;
        if (server.hotkeys->tracked_metrics & HOTKEYS_TRACK_CPU) {
            struct rusage current_ru;
            getrusage(RUSAGE_SELF, &current_ru);

            /* Calculate difference in user and sys time */
            long user_sec = current_ru.ru_utime.tv_sec -
                server.hotkeys->rusage_start.ru_utime.tv_sec;

            long user_usec = current_ru.ru_utime.tv_usec -
                server.hotkeys->rusage_start.ru_utime.tv_usec;

            if (user_usec < 0) {
                user_sec--;
                user_usec += 1000000;
            }
            total_cpu_user_msec = (user_sec * 1000) + (user_usec / 1000);
 
            long sys_sec = current_ru.ru_stime.tv_sec -
                server.hotkeys->rusage_start.ru_stime.tv_sec;

            long sys_usec = current_ru.ru_stime.tv_usec -
                server.hotkeys->rusage_start.ru_stime.tv_usec;

            if (sys_usec < 0) {
                sys_sec--;
                sys_usec += 1000000;
            }
            total_cpu_sys_msec = (sys_sec * 1000) + (sys_usec / 1000);
        }

        /* Get totals and lists for enabled metrics */
        uint64_t total_net_bytes = 0;
        chkHeapBucket *cpu = NULL;
        chkHeapBucket *net = NULL;
        int cpu_count = 0;
        int net_count = 0;

        if (server.hotkeys->tracked_metrics & HOTKEYS_TRACK_CPU) {
            cpu = chkTopKList(server.hotkeys->cpu);
            for (int i = 0; i < server.hotkeys->k; ++i) {
                if (cpu[i].count == 0) break;
                cpu_count++;
            }
        }

        if (server.hotkeys->tracked_metrics & HOTKEYS_TRACK_NET) {
            total_net_bytes = server.hotkeys->net->total;
            net = chkTopKList(server.hotkeys->net);
            for (int i = 0; i < server.hotkeys->k; ++i) {
                if (net[i].count == 0) break;
                net_count++;
            }
        }

        int has_selected_slots = (server.hotkeys->numslots > 0);
        int has_sampling = (server.hotkeys->sample_ratio > 1);

        int total_len = 14;
        void *arraylenptr = addReplyDeferredLen(c);

        /* tracking-active */
        addReplyBulkCString(c, "tracking-active");
        addReplyLongLong(c, server.hotkeys->active ? 1 : 0);

        /* sample-ratio */
        addReplyBulkCString(c, "sample-ratio");
        addReplyLongLong(c, server.hotkeys->sample_ratio);

        /* selected-slots */
        addReplyBulkCString(c, "selected-slots");
        addReplyArrayLen(c, server.hotkeys->numslots);
        for (int i = 0; i < server.hotkeys->numslots; i++) {
            addReplyLongLong(c, server.hotkeys->slots[i]);
        }

        /* sampled-command-selected-slots-ms (conditional) */
        if (has_sampling && has_selected_slots) {
            addReplyBulkCString(c, "sampled-command-selected-slots-ms");
            addReplyLongLong(c,
                server.hotkeys->time_sampled_commands_selected_slots / 1000);

            total_len += 2;
        }

        /* all-commands-selected-slots-ms (conditional) */
        if (has_selected_slots) {
            addReplyBulkCString(c, "all-commands-selected-slots-ms");
            addReplyLongLong(c,
                server.hotkeys->time_all_commands_selected_slots / 1000);

            total_len += 2;
        }

        /* all-commands-all-slots-ms */
        addReplyBulkCString(c, "all-commands-all-slots-ms");
        addReplyLongLong(c, server.hotkeys->time_all_commands_all_slots / 1000);

        /* net-bytes-sampled-commands-selected-slots (conditional) */
        if (has_sampling && has_selected_slots) {
            addReplyBulkCString(c, "net-bytes-sampled-commands-selected-slots");
            addReplyLongLong(c,
                server.hotkeys->net_bytes_sampled_commands_selected_slots);

            total_len += 2;
        }

        /* net-bytes-all-commands-selected-slots (conditional) */
        if (has_selected_slots) {
            addReplyBulkCString(c, "net-bytes-all-commands-selected-slots");
            addReplyLongLong(c,
                server.hotkeys->net_bytes_all_commands_selected_slots);

            total_len += 2;
        }

        /* net-bytes-all-commands-all-slots */
        addReplyBulkCString(c, "net-bytes-all-commands-all-slots");
        addReplyLongLong(c, server.hotkeys->net_bytes_all_commands_all_slots);

        /* collection-start-time-unix-ms */
        addReplyBulkCString(c, "collection-start-time-unix-ms");
        addReplyLongLong(c, server.hotkeys->start);

        /* collection-duration-ms */
        addReplyBulkCString(c, "collection-duration-ms");
        addReplyLongLong(c, duration);

        /* total-cpu-time-user-ms (in milliseconds) - only if CPU tracking is enabled */
        if (server.hotkeys->tracked_metrics & HOTKEYS_TRACK_CPU) {
            addReplyBulkCString(c, "total-cpu-time-user-ms");
            addReplyLongLong(c, total_cpu_user_msec);

            /* total-cpu-time-sys-ms (in milliseconds) */
            addReplyBulkCString(c, "total-cpu-time-sys-ms");
            addReplyLongLong(c, total_cpu_sys_msec);

            total_len += 4;
        }

        /* total-net-bytes - only if NET tracking is enabled */
        if (server.hotkeys->tracked_metrics & HOTKEYS_TRACK_NET) {
            addReplyBulkCString(c, "total-net-bytes");
            addReplyLongLong(c, total_net_bytes);

            total_len += 2;
        }

        /* by-cpu-time - only if CPU tracking is enabled */
        if (server.hotkeys->tracked_metrics & HOTKEYS_TRACK_CPU) {
            addReplyBulkCString(c, "by-cpu-time");
            /* Nested array of key-value pairs */
            addReplyArrayLen(c, 2 * cpu_count);
            for (int i = 0; i < cpu_count; ++i) {
                addReplyBulkCBuffer(c, cpu[i].item, cpu[i].itemlen);
                /* Return raw microsec value */
                addReplyLongLong(c, cpu[i].count);
            }
            zfree(cpu);

            total_len += 2;
        }

        /* by-net-bytes - only if NET tracking is enabled */
        if (server.hotkeys->tracked_metrics & HOTKEYS_TRACK_NET) {
            addReplyBulkCString(c, "by-net-bytes");
            /* Nested array of key-value pairs */
            addReplyArrayLen(c, 2 * net_count);
            for (int i = 0; i < net_count; ++i) {
                addReplyBulkCBuffer(c, net[i].item, net[i].itemlen);
                /* Return raw byte value */
                addReplyLongLong(c, net[i].count);
            }
            zfree(net);

            total_len += 2;
        }

        setDeferredArrayLen(c, arraylenptr, total_len);

    } else if (!strcasecmp(sub, "RESET")) {
        /* HOTKEYS RESET */
        if (c->argc != 2) {
            addReplyError(c,
                "wrong number of arguments for 'hotkeys|reset' command");
            return;
        }

        /* Return error if session is in progress and not yet completed */
        if (server.hotkeys && server.hotkeys->active) {
            addReplyError(c,
                "hotkey tracking session in progress, stop tracking first");
            return;
        }

        /* Release the resources used for hotkey tracking */
        hotkeyStatsRelease(server.hotkeys);
        server.hotkeys = NULL;
 
        addReply(c, shared.ok);
    } else {
        addReplyError(c, "unknown subcommand or wrong number of arguments");
    }
}
