#include "redismodule.h"
#include <string.h>

/* See moduleconfigs.c for registering module configs. We need to register some
 * module configs with our module in order to test the interaction between
 * module configs and the RM_ConfigGet/Set APIs. */
int serverconfig_bool;

int getBoolConfigCommand(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(name);
    return (*(int *)privdata);
}

int setBoolConfigCommand(const char *name, int new, void *privdata, RedisModuleString **err) {
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(err);
    *(int *)privdata = new;
    return REDISMODULE_OK;
}

/* Test command for RM_ConfigGet */
int TestConfigGet_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }
    
    size_t len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &len);
    
    RedisModuleString *value = RedisModule_ConfigGet(ctx, config_name);
    if (value == NULL) {
        RedisModule_ReplyWithError(ctx, "ERR Failed to get config");
    } else {
        RedisModule_ReplyWithString(ctx, value);
        RedisModule_FreeString(ctx, value);
    }
    
    return REDISMODULE_OK;
}

/* Test command for RM_ConfigSet */
int TestConfigSet_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }
    
    size_t name_len, value_len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &name_len);
    const char *config_value = RedisModule_StringPtrLen(argv[2], &value_len);
    
    int result = RedisModule_ConfigSet(ctx, config_name, config_value);
    if (result == REDISMODULE_OK) {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        RedisModule_ReplyWithError(ctx, "ERR Failed to set config");
    }
    
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "serverconfig", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "serverconfig.get", 
                                 TestConfigGet_RedisCommand, "readonly", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "serverconfig.set", 
                                 TestConfigSet_RedisCommand, "write", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_RegisterBoolConfig(ctx, "bool", 1, REDISMODULE_CONFIG_DEFAULT,
                                       getBoolConfigCommand, setBoolConfigCommand, NULL, &serverconfig_bool) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to register serverconfig_bool");
        return REDISMODULE_ERR;
    }

    RedisModule_Log(ctx, "debug", "Loading serverconfig module configuration");
    if (RedisModule_LoadConfigs(ctx) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to load serverconfig module configuration");
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}
