#include "valkeymodule.h"

#include <string.h>
#include <strings.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>

/* Command which adds a stream entry with automatic ID, like XADD *.
 *
 * Syntax: STREAM.ADD key field1 value1 [ field2 value2 ... ]
 *
 * The response is the ID of the added stream entry or an error message.
 */
int stream_add(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 2 || argc % 2 != 0) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    ValkeyModuleStreamID id;
    if (ValkeyModule_StreamAdd(key, VALKEYMODULE_STREAM_ADD_AUTOID, &id,
                              &argv[2], (argc-2)/2) == VALKEYMODULE_OK) {
        ValkeyModuleString *id_str = ValkeyModule_CreateStringFromStreamID(ctx, &id);
        ValkeyModule_ReplyWithString(ctx, id_str);
        ValkeyModule_FreeString(ctx, id_str);
    } else {
        ValkeyModule_ReplyWithError(ctx, "ERR StreamAdd failed");
    }
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* Command which adds a stream entry N times.
 *
 * Syntax: STREAM.ADD key N field1 value1 [ field2 value2 ... ]
 *
 * Returns the number of successfully added entries.
 */
int stream_addn(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 3 || argc % 2 == 0) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    long long n, i;
    if (ValkeyModule_StringToLongLong(argv[2], &n) == VALKEYMODULE_ERR) {
        ValkeyModule_ReplyWithError(ctx, "N must be a number");
        return VALKEYMODULE_OK;
    }

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    for (i = 0; i < n; i++) {
        if (ValkeyModule_StreamAdd(key, VALKEYMODULE_STREAM_ADD_AUTOID, NULL,
                                  &argv[3], (argc-3)/2) == VALKEYMODULE_ERR)
            break;
    }
    ValkeyModule_ReplyWithLongLong(ctx, i);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* STREAM.DELETE key stream-id */
int stream_delete(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) return ValkeyModule_WrongArity(ctx);
    ValkeyModuleStreamID id;
    if (ValkeyModule_StringToStreamID(argv[2], &id) != VALKEYMODULE_OK) {
        return ValkeyModule_ReplyWithError(ctx, "Invalid stream ID");
    }
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    if (ValkeyModule_StreamDelete(key, &id) == VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        ValkeyModule_ReplyWithError(ctx, "ERR StreamDelete failed");
    }
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* STREAM.RANGE key start-id end-id
 *
 * Returns an array of stream items. Each item is an array on the form
 * [stream-id, [field1, value1, field2, value2, ...]].
 *
 * A funny side-effect used for testing RM_StreamIteratorDelete() is that if any
 * entry has a field named "selfdestruct", the stream entry is deleted. It is
 * however included in the results of this command.
 */
int stream_range(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    ValkeyModuleStreamID startid, endid;
    if (ValkeyModule_StringToStreamID(argv[2], &startid) != VALKEYMODULE_OK ||
        ValkeyModule_StringToStreamID(argv[3], &endid) != VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "Invalid stream ID");
        return VALKEYMODULE_OK;
    }

    /* If startid > endid, we swap and set the reverse flag. */
    int flags = 0;
    if (startid.ms > endid.ms ||
        (startid.ms == endid.ms && startid.seq > endid.seq)) {
        ValkeyModuleStreamID tmp = startid;
        startid = endid;
        endid = tmp;
        flags |= VALKEYMODULE_STREAM_ITERATOR_REVERSE;
    }

    /* Open key and start iterator. */
    int openflags = VALKEYMODULE_READ | VALKEYMODULE_WRITE;
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], openflags);
    if (ValkeyModule_StreamIteratorStart(key, flags,
                                        &startid, &endid) != VALKEYMODULE_OK) {
        /* Key is not a stream, etc. */
        ValkeyModule_ReplyWithError(ctx, "ERR StreamIteratorStart failed");
        ValkeyModule_CloseKey(key);
        return VALKEYMODULE_OK;
    }

    /* Check error handling: Delete current entry when no current entry. */
    assert(ValkeyModule_StreamIteratorDelete(key) ==
           VALKEYMODULE_ERR);
    assert(errno == ENOENT);

    /* Check error handling: Fetch fields when no current entry. */
    assert(ValkeyModule_StreamIteratorNextField(key, NULL, NULL) ==
           VALKEYMODULE_ERR);
    assert(errno == ENOENT);

    /* Return array. */
    ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
    ValkeyModule_AutoMemory(ctx);
    ValkeyModuleStreamID id;
    long numfields;
    long len = 0;
    while (ValkeyModule_StreamIteratorNextID(key, &id,
                                            &numfields) == VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithArray(ctx, 2);
        ValkeyModuleString *id_str = ValkeyModule_CreateStringFromStreamID(ctx, &id);
        ValkeyModule_ReplyWithString(ctx, id_str);
        ValkeyModule_ReplyWithArray(ctx, numfields * 2);
        int delete = 0;
        ValkeyModuleString *field, *value;
        for (long i = 0; i < numfields; i++) {
            assert(ValkeyModule_StreamIteratorNextField(key, &field, &value) ==
                   VALKEYMODULE_OK);
            ValkeyModule_ReplyWithString(ctx, field);
            ValkeyModule_ReplyWithString(ctx, value);
            /* check if this is a "selfdestruct" field */
            size_t field_len;
            const char *field_str = ValkeyModule_StringPtrLen(field, &field_len);
            if (!strncmp(field_str, "selfdestruct", field_len)) delete = 1;
        }
        if (delete) {
            assert(ValkeyModule_StreamIteratorDelete(key) == VALKEYMODULE_OK);
        }
        /* check error handling: no more fields to fetch */
        assert(ValkeyModule_StreamIteratorNextField(key, &field, &value) ==
               VALKEYMODULE_ERR);
        assert(errno == ENOENT);
        len++;
    }
    ValkeyModule_ReplySetArrayLength(ctx, len);
    ValkeyModule_StreamIteratorStop(key);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/*
 * STREAM.TRIM key (MAXLEN (=|~) length | MINID (=|~) id)
 */
int stream_trim(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 5) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    /* Parse args */
    int trim_by_id = 0; /* 0 = maxlen, 1 = minid */
    long long maxlen;
    ValkeyModuleStreamID minid;
    size_t arg_len;
    const char *arg = ValkeyModule_StringPtrLen(argv[2], &arg_len);
    if (!strcasecmp(arg, "minid")) {
        trim_by_id = 1;
        if (ValkeyModule_StringToStreamID(argv[4], &minid) != VALKEYMODULE_OK) {
            ValkeyModule_ReplyWithError(ctx, "ERR Invalid stream ID");
            return VALKEYMODULE_OK;
        }
    } else if (!strcasecmp(arg, "maxlen")) {
        if (ValkeyModule_StringToLongLong(argv[4], &maxlen) == VALKEYMODULE_ERR) {
            ValkeyModule_ReplyWithError(ctx, "ERR Maxlen must be a number");
            return VALKEYMODULE_OK;
        }
    } else {
        ValkeyModule_ReplyWithError(ctx, "ERR Invalid arguments");
        return VALKEYMODULE_OK;
    }

    /* Approx or exact */
    int flags;
    arg = ValkeyModule_StringPtrLen(argv[3], &arg_len);
    if (arg_len == 1 && arg[0] == '~') {
        flags = VALKEYMODULE_STREAM_TRIM_APPROX;
    } else if (arg_len == 1 && arg[0] == '=') {
        flags = 0;
    } else {
        ValkeyModule_ReplyWithError(ctx, "ERR Invalid approx-or-exact mark");
        return VALKEYMODULE_OK;
    }

    /* Trim */
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    long long trimmed;
    if (trim_by_id) {
        trimmed = ValkeyModule_StreamTrimByID(key, flags, &minid);
    } else {
        trimmed = ValkeyModule_StreamTrimByLength(key, flags, maxlen);
    }

    /* Return result */
    if (trimmed < 0) {
        ValkeyModule_ReplyWithError(ctx, "ERR Trimming failed");
    } else {
        ValkeyModule_ReplyWithLongLong(ctx, trimmed);
    }
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, "stream", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "stream.add", stream_add, "write",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "stream.addn", stream_addn, "write",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "stream.delete", stream_delete, "write",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "stream.range", stream_range, "write",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "stream.trim", stream_trim, "write",
                                  1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
