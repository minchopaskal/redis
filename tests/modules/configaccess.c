#include "redismodule.h"
#include <string.h>

/* See moduleconfigs.c for registering module configs. We need to register some
 * module configs with our module in order to test the interaction between
 * module configs and the RM_Get/Set*Config APIs. */
int configaccess_bool;

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

/* Test command for RM_GetBoolConfig */
int TestGetBoolConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    size_t len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &len);

    int value;
    if (RedisModule_GetBoolConfig(ctx, config_name, &value) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "ERR Failed to get bool config");
        return REDISMODULE_ERR;
    }

    RedisModule_ReplyWithLongLong(ctx, value);
    return REDISMODULE_OK;
}

/* Test command for RM_GetNumericConfig */
int TestGetNumericConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    size_t len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &len);

    long long value;
    if (RedisModule_GetNumericConfig(ctx, config_name, &value) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "ERR Failed to get numeric config");
        return REDISMODULE_ERR;
    }

    RedisModule_ReplyWithLongLong(ctx, value);
    return REDISMODULE_OK;
}

/* Test command for RM_GetStringConfig */
int TestGetStringConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    size_t len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &len);

    RedisModuleString *value;
    if (RedisModule_GetStringConfig(ctx, config_name, &value) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "ERR Failed to get string config");
        return REDISMODULE_ERR;
    }

    RedisModule_ReplyWithString(ctx, value);
    RedisModule_FreeString(ctx,value);
    return REDISMODULE_OK;
}

/* Test command for RM_GetEnumConfig with integer value */
int TestGetEnumValConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    size_t len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &len);

    int value;
    if (RedisModule_GetEnumConfigValue(ctx, config_name, &value) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "ERR Failed to get enum value config");
        return REDISMODULE_ERR;
    }

    RedisModule_ReplyWithLongLong(ctx, value);
    return REDISMODULE_OK;
}

/* Test command for RM_GetEnumConfig with name */
int TestGetEnumNameConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    size_t len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &len);

    RedisModuleString *value;
    if (RedisModule_GetEnumConfigName(ctx, config_name, &value) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "ERR Failed to get enum name config");
        return REDISMODULE_ERR;
    }

    RedisModule_ReplyWithString(ctx, value);
    RedisModule_Free(value);
    return REDISMODULE_OK;
}

/* Test command for RM_SetBoolConfig */
int TestSetBoolConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    size_t name_len, value_len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &name_len);
    const char *config_value = RedisModule_StringPtrLen(argv[2], &value_len);

    int bool_value;
    if (!strcasecmp(config_value, "yes") || !strcasecmp(config_value, "1") || 
        !strcasecmp(config_value, "true") || !strcasecmp(config_value, "on")) {
        bool_value = 1;
    } else {
        bool_value = 0;
    }

    RedisModuleString *error = NULL;
    int result = RedisModule_SetBoolConfig(ctx, config_name, bool_value, &error);
    if (result == REDISMODULE_OK) {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        RedisModule_ReplyWithErrorFormat(ctx, "ERR Failed to set bool config %s: %s", config_name, RedisModule_StringPtrLen(error, NULL));
        RedisModule_FreeString(ctx, error);
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

/* Test command for RM_SetNumericConfig */
int TestSetNumericConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    size_t name_len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &name_len);

    long long value;
    if (RedisModule_StringToLongLong(argv[2], &value) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "ERR Invalid numeric value");
        return REDISMODULE_ERR;
    }

    RedisModuleString *error = NULL;
    int result = RedisModule_SetNumericConfig(ctx, config_name, value, &error);
    if (result == REDISMODULE_OK) {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        RedisModule_ReplyWithErrorFormat(ctx, "ERR Failed to set numeric config %s: %s", config_name, RedisModule_StringPtrLen(error, NULL));
        RedisModule_FreeString(ctx, error);
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

/* Test command for RM_SetStringConfig */
int TestSetStringConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    size_t name_len, value_len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &name_len);
    const char *config_value = RedisModule_StringPtrLen(argv[2], &value_len);

    RedisModuleString *error = NULL;
    int result = RedisModule_SetStringConfig(ctx, config_name, config_value, &error);
    if (result == REDISMODULE_OK) {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        RedisModule_ReplyWithErrorFormat(ctx, "ERR Failed to set string config %s: %s", config_name, RedisModule_StringPtrLen(error, NULL));
        RedisModule_FreeString(ctx, error);
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

/* Test command for RM_SetEnumConfig with integer value */
int TestSetEnumValConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    size_t name_len;
    const char *config_name = RedisModule_StringPtrLen(argv[1], &name_len);

    long long value;
    if (RedisModule_StringToLongLong(argv[2], &value) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "ERR Invalid numeric value");
        return REDISMODULE_ERR;
    }

    RedisModuleString *error = NULL;
    int result = RedisModule_SetEnumConfigWithValue(ctx, config_name, (int)value, &error);
    if (result == REDISMODULE_OK) {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        RedisModule_ReplyWithErrorFormat(ctx, "ERR Failed to set enum config %s: %s", config_name, RedisModule_StringPtrLen(error, NULL));
        RedisModule_FreeString(ctx, error);
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

/* Test command for RM_SetEnumConfig with name */
int TestSetEnumNameConfig_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }

    const char *config_name = RedisModule_StringPtrLen(argv[1], NULL);

    const char **values = RedisModule_Alloc(sizeof(char*)*(argc - 2));
    for (int i = 2; i < argc; i++) {
        values[i - 2] = RedisModule_StringPtrLen(argv[i], NULL);
    }

    RedisModuleString *error = NULL;
    int result = RedisModule_SetEnumConfigWithName(ctx, config_name, values, argc - 2, &error);
    RedisModule_Free(values);

    if (result == REDISMODULE_OK) {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        RedisModule_ReplyWithErrorFormat(ctx, "ERR Failed to set enum config %s: %s", config_name, RedisModule_StringPtrLen(error, NULL));
        RedisModule_FreeString(ctx, error);
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "configaccess", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    // Register commands for testing Get*Config functions
    if (RedisModule_CreateCommand(ctx, "configaccess.getbool", 
                                 TestGetBoolConfig_RedisCommand, "readonly", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "configaccess.getnumeric", 
                                 TestGetNumericConfig_RedisCommand, "readonly", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "configaccess.getstring", 
                                 TestGetStringConfig_RedisCommand, "readonly", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "configaccess.getenumval", 
                                 TestGetEnumValConfig_RedisCommand, "readonly", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "configaccess.getenumname", 
                                 TestGetEnumNameConfig_RedisCommand, "readonly", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    // Register commands for testing Set*Config functions
    if (RedisModule_CreateCommand(ctx, "configaccess.setbool", 
                                 TestSetBoolConfig_RedisCommand, "write", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "configaccess.setnumeric", 
                                 TestSetNumericConfig_RedisCommand, "write", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "configaccess.setstring", 
                                 TestSetStringConfig_RedisCommand, "write", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "configaccess.setenumval", 
                                 TestSetEnumValConfig_RedisCommand, "write", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "configaccess.setenumname", 
                                 TestSetEnumNameConfig_RedisCommand, "write", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_RegisterBoolConfig(ctx, "bool", 1, REDISMODULE_CONFIG_DEFAULT,
                                       getBoolConfigCommand, setBoolConfigCommand, NULL, &configaccess_bool) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to register configaccess_bool");
        return REDISMODULE_ERR;
    }

    RedisModule_Log(ctx, "debug", "Loading configaccess module configuration");
    if (RedisModule_LoadConfigs(ctx) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Failed to load configaccess module configuration");
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}
