/* define macros for having usleep */
#define _BSD_SOURCE
#define _DEFAULT_SOURCE

#include "redismodule.h"

#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define UNUSED(V) ((void) V)

#include <time.h>

static int is_leap_year(time_t year) {
    if (year % 4) return 0;         /* A year not divisible by 4 is not leap. */
    else if (year % 100) return 1;  /* If div by 4 and not 100 is surely leap. */
    else if (year % 400) return 0;  /* If div by 100 *and* not by 400 is not leap. */
    else return 1;                  /* If div by 100 and 400 is leap. */
}

void nolocks_localtime(struct tm *tmp, time_t t, time_t tz, int dst) {
    const time_t secs_min = 60;
    const time_t secs_hour = 3600;
    const time_t secs_day = 3600*24;

    t -= tz;                            /* Adjust for timezone. */
    t += 3600*dst;                      /* Adjust for daylight time. */
    time_t days = t / secs_day;         /* Days passed since epoch. */
    time_t seconds = t % secs_day;      /* Remaining seconds. */

    tmp->tm_isdst = dst;
    tmp->tm_hour = seconds / secs_hour;
    tmp->tm_min = (seconds % secs_hour) / secs_min;
    tmp->tm_sec = (seconds % secs_hour) % secs_min;

    /* 1/1/1970 was a Thursday, that is, day 4 from the POV of the tm structure
     * where sunday = 0, so to calculate the day of the week we have to add 4
     * and take the modulo by 7. */
    tmp->tm_wday = (days+4)%7;

    /* Calculate the current year. */
    tmp->tm_year = 1970;
    while(1) {
        /* Leap years have one day more. */
        time_t days_this_year = 365 + is_leap_year(tmp->tm_year);
        if (days_this_year > days) break;
        days -= days_this_year;
        tmp->tm_year++;
    }
    tmp->tm_yday = days;  /* Number of day of the current year. */

    /* We need to calculate in which month and day of the month we are. To do
     * so we need to skip days according to how many days there are in each
     * month, and adjust for the leap year that has one more day in February. */
    int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    mdays[1] += is_leap_year(tmp->tm_year);

    tmp->tm_mon = 0;
    while(days >= mdays[tmp->tm_mon]) {
        days -= mdays[tmp->tm_mon];
        tmp->tm_mon++;
    }

    tmp->tm_mday = days+1;  /* Add 1 since our 'days' is zero-based. */
    tmp->tm_year -= 1900;   /* Surprisingly tm_year is year-1900. */
}

// A simple global user
static RedisModuleUser *global = NULL;
static long long client_change_delta = 0;

void UserChangedCallback(uint64_t client_id, void *privdata) {
    REDISMODULE_NOT_USED(privdata);
    REDISMODULE_NOT_USED(client_id);
    client_change_delta++;
}

int Auth_CreateModuleUser(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (global) {
        RedisModule_FreeModuleUser(global);
    }

    global = RedisModule_CreateModuleUser("global");
    RedisModule_SetModuleUserACL(global, "allcommands");
    RedisModule_SetModuleUserACL(global, "allkeys");
    RedisModule_SetModuleUserACL(global, "on");

    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

int Auth_AuthModuleUser(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    uint64_t client_id;
    RedisModule_AuthenticateClientWithUser(ctx, global, UserChangedCallback, NULL, &client_id);

    return RedisModule_ReplyWithLongLong(ctx, (uint64_t) client_id);
}

int Auth_AuthRealUser(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    size_t length;
    uint64_t client_id;

    RedisModuleString *user_string = argv[1];
    const char *name = RedisModule_StringPtrLen(user_string, &length);

    if (RedisModule_AuthenticateClientWithACLUser(ctx, name, length, 
            UserChangedCallback, NULL, &client_id) == REDISMODULE_ERR) {
        return RedisModule_ReplyWithError(ctx, "Invalid user");   
    }

    return RedisModule_ReplyWithLongLong(ctx, (uint64_t) client_id);
}

/* This command redacts every other arguments and returns OK */
int Auth_RedactedAPI(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    for(int i = argc - 1; i > 0; i -= 2) {
        int result = RedisModule_RedactClientCommandArgument(ctx, i);
        RedisModule_Assert(result == REDISMODULE_OK);
    }
    return RedisModule_ReplyWithSimpleString(ctx, "OK"); 
}

int Auth_ChangeCount(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    long long result = client_change_delta;
    client_change_delta = 0;
    return RedisModule_ReplyWithLongLong(ctx, result);
}

/* The Module functionality below validates that module authentication callbacks can be registered
 * to support both non-blocking and blocking module based authentication. */

/* Non Blocking Module Auth callback / implementation. */
int auth_cb(RedisModuleCtx *ctx, RedisModuleString *username, RedisModuleString *password, RedisModuleString **err) {
    const char *user = RedisModule_StringPtrLen(username, NULL);
    const char *pwd = RedisModule_StringPtrLen(password, NULL);
    if (!strcmp(user,"foo") && !strcmp(pwd,"allow")) {
        RedisModule_AuthenticateClientWithACLUser(ctx, "foo", 3, NULL, NULL, NULL);
        return REDISMODULE_AUTH_HANDLED;
    }
    else if (!strcmp(user,"foo") && !strcmp(pwd,"deny")) {
        RedisModuleString *log = RedisModule_CreateString(ctx, "Module Auth", 11);
        RedisModule_ACLAddLogEntryByUserName(ctx, username, log, REDISMODULE_ACL_LOG_AUTH);
        RedisModule_FreeString(ctx, log);
        const char *err_msg = "Auth denied by Misc Module.";
        *err = RedisModule_CreateString(ctx, err_msg, strlen(err_msg));
        return REDISMODULE_AUTH_HANDLED;
    }
    return REDISMODULE_AUTH_NOT_HANDLED;
}

int test_rm_register_auth_cb(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModule_RegisterAuthCallback(ctx, auth_cb);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/*
 * The thread entry point that actually executes the blocking part of the AUTH command.
 * This function sleeps for 0.5 seconds and then unblocks the client which will later call
 * `AuthBlock_Reply`.
 * `arg` is expected to contain the RedisModuleBlockedClient, username, and password.
 */
long getTimeZone(void) {
#if defined(__linux__) || defined(__sun)
    return timezone;
#else
    struct timezone tz;

    gettimeofday(NULL, &tz);

    return tz.tz_minuteswest * 60L;
#endif
}

void *AuthBlock_ThreadMain(void *arg) {
    char buf[64];
    struct timeval tv;
    gettimeofday(&tv,NULL);
    struct tm tm;
    nolocks_localtime(&tm,tv.tv_sec,getTimeZone(),1);
    strftime(buf,sizeof(buf),"%d %b %Y %H:%M:%S.",&tm);
    printf("%s: AUTH BLOCK THREAD\n", buf);
    fflush(stdout);

    usleep(500000);
    void **targ = arg;
    RedisModuleBlockedClient *bc = targ[0];
    int result = 2;
    const char *user = RedisModule_StringPtrLen(targ[1], NULL);
    const char *pwd = RedisModule_StringPtrLen(targ[2], NULL);
    if (!strcmp(user,"foo") && !strcmp(pwd,"block_allow")) {
        result = 1;
        printf("AUTH BLOCK THREAD 1\n");
    }
    else if (!strcmp(user,"foo") && !strcmp(pwd,"block_deny")) {
        printf("AUTH BLOCK THREAD 0\n");
        result = 0;
    }
    else if (!strcmp(user,"foo") && !strcmp(pwd,"block_abort")) {
        printf("AUTH BLOCK THREAD ABORT\n");
        RedisModule_BlockedClientMeasureTimeEnd(bc);
        RedisModule_AbortBlock(bc);
        goto cleanup;
    }
    /* Provide the result to the blocking reply cb. */
    void **replyarg = RedisModule_Alloc(sizeof(void*));
    replyarg[0] = (void *) (uintptr_t) result;
    RedisModule_BlockedClientMeasureTimeEnd(bc);
    RedisModule_UnblockClient(bc, replyarg);

    gettimeofday(&tv,NULL);
    nolocks_localtime(&tm,tv.tv_sec,getTimeZone(),1);
    strftime(buf,sizeof(buf),"%d %b %Y %H:%M:%S.",&tm);
    printf("%s: AUTH BLOCK THREAD ABORT unblocked!!!!\n", buf);
    fflush(stdout);
cleanup:
    /* Free the username and password and thread / arg data. */
    RedisModule_FreeString(NULL, targ[1]);
    RedisModule_FreeString(NULL, targ[2]);
    RedisModule_Free(targ);
    return NULL;
}

/*
 * Reply callback for a blocking AUTH command. This is called when the client is unblocked.
 */
int AuthBlock_Reply(RedisModuleCtx *ctx, RedisModuleString *username, RedisModuleString *password, RedisModuleString **err) {
    REDISMODULE_NOT_USED(password);
    void **targ = RedisModule_GetBlockedClientPrivateData(ctx);
    int result = (uintptr_t) targ[0];
    size_t userlen = 0;
    const char *user = RedisModule_StringPtrLen(username, &userlen);
    /* Handle the success case by authenticating. */
    if (result == 1) {
        RedisModule_AuthenticateClientWithACLUser(ctx, user, userlen, NULL, NULL, NULL);
        return REDISMODULE_AUTH_HANDLED;
    }
    /* Handle the Error case by denying auth */
    else if (result == 0) {
        RedisModuleString *log = RedisModule_CreateString(ctx, "Module Auth", 11);
        RedisModule_ACLAddLogEntryByUserName(ctx, username, log, REDISMODULE_ACL_LOG_AUTH);
        RedisModule_FreeString(ctx, log);
        const char *err_msg = "Auth denied by Misc Module.";
        *err = RedisModule_CreateString(ctx, err_msg, strlen(err_msg));
        return REDISMODULE_AUTH_HANDLED;
    }
    /* "Skip" Authentication */
    return REDISMODULE_AUTH_NOT_HANDLED;
}

/* Private data freeing callback for Module Auth. */
void AuthBlock_FreeData(RedisModuleCtx *ctx, void *privdata) {
    REDISMODULE_NOT_USED(ctx);
    RedisModule_Free(privdata);
}

/* Callback triggered when the engine attempts module auth
 * Return code here is one of the following: Auth succeeded, Auth denied,
 * Auth not handled, Auth blocked.
 * The Module can have auth succeed / denied here itself, but this is an example
 * of blocking module auth.
 */
int blocking_auth_cb(RedisModuleCtx *ctx, RedisModuleString *username, RedisModuleString *password, RedisModuleString **err) {
    printf("BLOCKING AUTH CB");
    REDISMODULE_NOT_USED(username);
    REDISMODULE_NOT_USED(password);
    REDISMODULE_NOT_USED(err);
    /* Block the client from the Module. */
    RedisModuleBlockedClient *bc = RedisModule_BlockClientOnAuth(ctx, AuthBlock_Reply, AuthBlock_FreeData);
    int ctx_flags = RedisModule_GetContextFlags(ctx);
    if (ctx_flags & REDISMODULE_CTX_FLAGS_MULTI || ctx_flags & REDISMODULE_CTX_FLAGS_LUA) {
        /* Clean up by using RedisModule_UnblockClient since we attempted blocking the client. */
        RedisModule_UnblockClient(bc, NULL);
        return REDISMODULE_AUTH_HANDLED;
    }
    RedisModule_BlockedClientMeasureTimeStart(bc);
    pthread_t tid;
    /* Allocate memory for information needed. */
    void **targ = RedisModule_Alloc(sizeof(void*)*3);
    targ[0] = bc;
    targ[1] = RedisModule_CreateStringFromString(NULL, username);
    targ[2] = RedisModule_CreateStringFromString(NULL, password);
    /* Create bg thread and pass the blockedclient, username and password to it. */
    if (pthread_create(&tid, NULL, AuthBlock_ThreadMain, targ) != 0) {
        RedisModule_AbortBlock(bc);
    }
    pthread_detach(tid);
    return REDISMODULE_AUTH_HANDLED;
}

int test_rm_register_blocking_auth_cb(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModule_RegisterAuthCallback(ctx, blocking_auth_cb);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"testacl",1,REDISMODULE_APIVER_1)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"auth.authrealuser",
        Auth_AuthRealUser,"no-auth",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"auth.createmoduleuser",
        Auth_CreateModuleUser,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"auth.authmoduleuser",
        Auth_AuthModuleUser,"no-auth",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"auth.changecount",
        Auth_ChangeCount,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"auth.redact",
        Auth_RedactedAPI,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"testmoduleone.rm_register_auth_cb",
        test_rm_register_auth_cb,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"testmoduleone.rm_register_blocking_auth_cb",
        test_rm_register_blocking_auth_cb,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    UNUSED(ctx);

    if (global)
        RedisModule_FreeModuleUser(global);

    return REDISMODULE_OK;
}
