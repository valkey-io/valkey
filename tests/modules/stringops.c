/* Module test for the string APIs:
 *   - ValkeyModule_CreateStringUninitialized
 *   - ValkeyModule_CreateStringReferenceFromKey
 *   - ValkeyModule_StringSetMove */
#include "valkeymodule.h"
#include <string.h>

/* Captured from a key's value and held across commands to assert a
 * CreateStringReferenceFromKey result is an independently-owned reference
 * that survives deletion/overwrite of the originating key.
 * The point is for modules to be able to hold onto a value snapshot without
 * requiring the gil. This static is a toy example of doing that. */
static ValkeyModuleString *captured_reference = NULL;

/* STRINGOPS.CREATE_UNINIT key value
 *
 * Creates an uninitialized string, fills it with 'value', and stores it in
 * 'key' with a copying StringSet. */
int create_uninit_string_set(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    size_t len;
    const char *value = ValkeyModule_StringPtrLen(argv[2], &len);

    /* make a string via the uninitialized path, wired up to automemory */
    ValkeyModuleString *str = ValkeyModule_CreateStringUninitialized(ctx, len);
    size_t str_len;
    char *buf = (char *)ValkeyModule_StringPtrLen(str, &str_len);
    if (str_len != len) return ValkeyModule_ReplyWithError(ctx, "ERR CreateStringUninitialized allocated the wrong length");
    memcpy(buf, value, len);

    // StringSet bumps refcount to 2 then copies into db
    if (ValkeyModule_StringSet(key, str) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR StringSet failed");
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* STRINGOPS.SETMOVE key value
 *
 * Creates an uninitialized string, fills it with 'value', and stores it in
 * 'key' with a memory-moving StringSetMove.
 * Auto memory is enabled here, so if the refcounts are bugged it will segfault.
 * */
int set_move(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    size_t len;
    const char *value = ValkeyModule_StringPtrLen(argv[2], &len);

    ValkeyModuleString *str = ValkeyModule_CreateStringUninitialized(ctx, len);
    size_t str_len;
    char *buf = (char *)ValkeyModule_StringPtrLen(str, &str_len);
    if (str_len != len) return ValkeyModule_ReplyWithError(ctx, "ERR CreateStringUninitialized allocated the wrong length");
    memcpy(buf, value, len);

    /* str should have refcount = 1, and follow-up access of key should still work */
    if (ValkeyModule_StringSetMove(key, str) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR StringSetMove failed");
    /* On return the auto-memory collector runs. If str was freed, it could blow up here */
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* STRINGOPS.SETMOVE_READONLY key value
 *
 * Creates an uninitialized string, fills it with 'value', and attempts to store
 * it in 'key' with a memory-moving StringSetMove.
 * `key` is opened as read though.
 * */
int set_move_on_readonly_key(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    size_t len;
    const char *value = ValkeyModule_StringPtrLen(argv[2], &len);

    ValkeyModuleString *str = ValkeyModule_CreateStringUninitialized(ctx, len);
    size_t str_len;
    char *buf = (char *)ValkeyModule_StringPtrLen(str, &str_len);
    if (str_len != len) return ValkeyModule_ReplyWithError(ctx, "ERR CreateStringUninitialized allocated the wrong length");
    memcpy(buf, value, len);

    if (ValkeyModule_StringSetMove(key, str) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "REFUSED");
    return ValkeyModule_ReplyWithSimpleString(ctx, "ERR StringSetMove worked against a key opened as READ");
}

/* STRINGOPS.SETMOVE_FAIL key
 *
 * Attempts to move a string whose refcount is > 1 into 'key'. StringSetMove
 * must refuse this. Replies "REFUSED" on the expected refusal, "MOVED" if the
 * guard failed to fire. */
int setmove_fail(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    /* No auto memory. RetainString bumps the refcount rather than dropping the string from the cleanup list. */
    ValkeyModuleString *str = ValkeyModule_CreateString(ctx, "shared", 6); /* refcount 1 */
    ValkeyModule_RetainString(ctx, str); /* refcount 2 */

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    int result = ValkeyModule_StringSetMove(key, str);
    ValkeyModule_CloseKey(key);

    const char *reply = (result == VALKEYMODULE_ERR) ? "REFUSED" : "MOVED";
    ValkeyModule_FreeString(ctx, str); /* refcount 1 */
    ValkeyModule_FreeString(ctx, str); /* refcount 0 */
    return ValkeyModule_ReplyWithSimpleString(ctx, reply);
}

/* STRINGOPS.GETREF key
 *
 * It's like get, but done with a refcounted string.
 * CreateStringReferenceFromKey and replies with its contents. */
int get_ref(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    ValkeyModuleString *reference = ValkeyModule_CreateStringReferenceFromKey(ctx, key); /* bumps the value's reference count up by 1 */
    if (reference == NULL) return ValkeyModule_ReplyWithNull(ctx);

    ValkeyModule_ReplyWithString(ctx, reference);
    ValkeyModule_FreeString(ctx, reference); /* references wasn't tracked by auto memory */
    return VALKEYMODULE_OK;
}

/* STRINGOPS.REF_CAPTURE key
 *
 * Captures a reference to a value into a module global variable.
 * This is for reading the value later after changing the key that pointed to it. */
int ref_capture(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);

    /* just so the tests can be nicer, maintain the reference refcount */
    if (captured_reference != NULL) {
        ValkeyModule_FreeString(NULL, captured_reference);
        captured_reference = NULL;
    }

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    captured_reference = ValkeyModule_CreateStringReferenceFromKey(ctx, key);

    if (captured_reference == NULL) return ValkeyModule_ReplyWithNull(ctx);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* STRINGOPS.REF_READ -- Read the previously ref_captured value */
int ref_read(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (captured_reference == NULL) return ValkeyModule_ReplyWithNull(ctx);
    return ValkeyModule_ReplyWithString(ctx, captured_reference);
}

/* STRINGOPS.REF_RELEASE -- Free the ref_captured value */
int ref_release(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (captured_reference != NULL) {
        ValkeyModule_FreeString(NULL, captured_reference);
        captured_reference = NULL;
    }
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, "stringops", 1, VALKEYMODULE_APIVER_1) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "stringops.create_uninit_string_set", create_uninit_string_set, "write", 1, 1, 1) == VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "stringops.setmove", set_move, "write", 1, 1, 1) == VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "stringops.setmove_on_readonly_key", set_move_on_readonly_key, "readonly", 1, 1, 1) == VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "stringops.setmove_fail", setmove_fail, "write", 1, 1, 1) == VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "stringops.getref", get_ref, "readonly", 1, 1, 1) == VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "stringops.ref_capture", ref_capture, "readonly", 1, 1, 1) == VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "stringops.ref_read", ref_read, "readonly", 0, 0, 0) == VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "stringops.ref_release", ref_release, "readonly", 0, 0, 0) == VALKEYMODULE_OK) {
        return VALKEYMODULE_OK;
    }
    return VALKEYMODULE_ERR;
}

int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    VALKEYMODULE_NOT_USED(ctx);
    if (captured_reference != NULL) {
        ValkeyModule_FreeString(NULL, captured_reference);
        captured_reference = NULL;
    }
    return VALKEYMODULE_OK;
}
