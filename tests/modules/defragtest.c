/* A module that implements defrag callback mechanisms.
 */

#include "valkeymodule.h"
#include <stdlib.h>

static ValkeyModuleType *FragType;

struct FragObject {
    unsigned long len;
    void **values;
    int maxstep;
};

/* Make sure we get the expected cursor */
unsigned long int last_set_cursor = 0;

unsigned long int datatype_attempts = 0;
unsigned long int datatype_defragged = 0;
unsigned long int datatype_resumes = 0;
unsigned long int datatype_wrong_cursor = 0;
unsigned long int global_attempts = 0;
unsigned long int global_defragged = 0;

int global_strings_len = 0;
ValkeyModuleString **global_strings = NULL;

static void createGlobalStrings(ValkeyModuleCtx *ctx, int count)
{
    global_strings_len = count;
    global_strings = ValkeyModule_Alloc(sizeof(ValkeyModuleString *) * count);

    for (int i = 0; i < count; i++) {
        global_strings[i] = ValkeyModule_CreateStringFromLongLong(ctx, i);
    }
}

static void defragGlobalStrings(ValkeyModuleDefragCtx *ctx)
{
    for (int i = 0; i < global_strings_len; i++) {
        ValkeyModuleString *new = ValkeyModule_DefragValkeyModuleString(ctx, global_strings[i]);
        global_attempts++;
        if (new != NULL) {
            global_strings[i] = new;
            global_defragged++;
        }
    }
}

static void FragInfo(ValkeyModuleInfoCtx *ctx, int for_crash_report) {
    VALKEYMODULE_NOT_USED(for_crash_report);

    ValkeyModule_InfoAddSection(ctx, "stats");
    ValkeyModule_InfoAddFieldLongLong(ctx, "datatype_attempts", datatype_attempts);
    ValkeyModule_InfoAddFieldLongLong(ctx, "datatype_defragged", datatype_defragged);
    ValkeyModule_InfoAddFieldLongLong(ctx, "datatype_resumes", datatype_resumes);
    ValkeyModule_InfoAddFieldLongLong(ctx, "datatype_wrong_cursor", datatype_wrong_cursor);
    ValkeyModule_InfoAddFieldLongLong(ctx, "global_attempts", global_attempts);
    ValkeyModule_InfoAddFieldLongLong(ctx, "global_defragged", global_defragged);
}

struct FragObject *createFragObject(unsigned long len, unsigned long size, int maxstep) {
    struct FragObject *o = ValkeyModule_Alloc(sizeof(*o));
    o->len = len;
    o->values = ValkeyModule_Alloc(sizeof(ValkeyModuleString*) * len);
    o->maxstep = maxstep;

    for (unsigned long i = 0; i < len; i++) {
        o->values[i] = ValkeyModule_Calloc(1, size);
    }

    return o;
}

/* FRAG.RESETSTATS */
static int fragResetStatsCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    datatype_attempts = 0;
    datatype_defragged = 0;
    datatype_resumes = 0;
    datatype_wrong_cursor = 0;
    global_attempts = 0;
    global_defragged = 0;

    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

/* FRAG.CREATE key len size maxstep */
static int fragCreateCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 5)
        return ValkeyModule_WrongArity(ctx);

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx,argv[1],
                                              VALKEYMODULE_READ|VALKEYMODULE_WRITE);
    int type = ValkeyModule_KeyType(key);
    if (type != VALKEYMODULE_KEYTYPE_EMPTY)
    {
        return ValkeyModule_ReplyWithError(ctx, "ERR key exists");
    }

    long long len;
    if ((ValkeyModule_StringToLongLong(argv[2], &len) != VALKEYMODULE_OK)) {
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid len");
    }

    long long size;
    if ((ValkeyModule_StringToLongLong(argv[3], &size) != VALKEYMODULE_OK)) {
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid size");
    }

    long long maxstep;
    if ((ValkeyModule_StringToLongLong(argv[4], &maxstep) != VALKEYMODULE_OK)) {
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid maxstep");
    }

    struct FragObject *o = createFragObject(len, size, maxstep);
    ValkeyModule_ModuleTypeSetValue(key, FragType, o);
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    ValkeyModule_CloseKey(key);

    return VALKEYMODULE_OK;
}

void FragFree(void *value) {
    struct FragObject *o = value;

    for (unsigned long i = 0; i < o->len; i++)
        ValkeyModule_Free(o->values[i]);
    ValkeyModule_Free(o->values);
    ValkeyModule_Free(o);
}

size_t FragFreeEffort(ValkeyModuleString *key, const void *value) {
    VALKEYMODULE_NOT_USED(key);

    const struct FragObject *o = value;
    return o->len;
}

int FragDefrag(ValkeyModuleDefragCtx *ctx, ValkeyModuleString *key, void **value) {
    VALKEYMODULE_NOT_USED(key);
    unsigned long i = 0;
    int steps = 0;

    int dbid = ValkeyModule_GetDbIdFromDefragCtx(ctx);
    ValkeyModule_Assert(dbid != -1);

    /* Attempt to get cursor, validate it's what we're exepcting */
    if (ValkeyModule_DefragCursorGet(ctx, &i) == VALKEYMODULE_OK) {
        if (i > 0) datatype_resumes++;

        /* Validate we're expecting this cursor */
        if (i != last_set_cursor) datatype_wrong_cursor++;
    } else {
        if (last_set_cursor != 0) datatype_wrong_cursor++;
    }

    /* Attempt to defrag the object itself */
    datatype_attempts++;
    struct FragObject *o = ValkeyModule_DefragAlloc(ctx, *value);
    if (o == NULL) {
        /* Not defragged */
        o = *value;
    } else {
        /* Defragged */
        *value = o;
        datatype_defragged++;
    }

    /* Deep defrag now */
    for (; i < o->len; i++) {
        datatype_attempts++;
        void *new = ValkeyModule_DefragAlloc(ctx, o->values[i]);
        if (new) {
            o->values[i] = new;
            datatype_defragged++;
        }

        if ((o->maxstep && ++steps > o->maxstep) ||
            ((i % 64 == 0) && ValkeyModule_DefragShouldStop(ctx)))
        {
            ValkeyModule_DefragCursorSet(ctx, i);
            last_set_cursor = i;
            return 1;
        }
    }

    last_set_cursor = 0;
    return 0;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "defragtest", 1, VALKEYMODULE_APIVER_1)
        == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    if (ValkeyModule_GetTypeMethodVersion() < VALKEYMODULE_TYPE_METHOD_VERSION) {
        return VALKEYMODULE_ERR;
    }

    long long glen;
    if (argc != 1 || ValkeyModule_StringToLongLong(argv[0], &glen) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    createGlobalStrings(ctx, glen);

    ValkeyModuleTypeMethods tm = {
            .version = VALKEYMODULE_TYPE_METHOD_VERSION,
            .free = FragFree,
            .free_effort = FragFreeEffort,
            .defrag = FragDefrag
    };

    FragType = ValkeyModule_CreateDataType(ctx, "frag_type", 0, &tm);
    if (FragType == NULL) return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "frag.create",
                                  fragCreateCommand, "write deny-oom", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "frag.resetstats",
                                  fragResetStatsCommand, "write deny-oom", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    ValkeyModule_RegisterInfoFunc(ctx, FragInfo);
    ValkeyModule_RegisterDefragFunc(ctx, defragGlobalStrings);

    return VALKEYMODULE_OK;
}
