#include <adapters/valkeymoduleapi.h>
#include <async.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <valkey.h>

void debugCallback(valkeyAsyncContext *c, void *r, void *privdata) {
    (void)privdata; //unused
    valkeyReply *reply = r;
    if (reply == NULL) {
        /* The DEBUG SLEEP command will almost always fail, because we have set a 1 second timeout */
        printf("`DEBUG SLEEP` error: %s\n", c->errstr ? c->errstr : "unknown error");
        return;
    }
    /* Disconnect after receiving the reply of DEBUG SLEEP (which will not)*/
    valkeyAsyncDisconnect(c);
}

void getCallback(valkeyAsyncContext *c, void *r, void *privdata) {
    valkeyReply *reply = r;
    if (reply == NULL) {
        if (c->errstr) {
            printf("errstr: %s\n", c->errstr);
        }
        return;
    }
    printf("argv[%s]: %s\n", (char *)privdata, reply->str);

    /* start another request that demonstrate timeout */
    valkeyAsyncCommand(c, debugCallback, NULL, "DEBUG SLEEP %f", 1.5);
}

void connectCallback(valkeyAsyncContext *c, int status) {
    if (status != VALKEY_OK) {
        printf("Error: %s\n", c->errstr);
        return;
    }
    printf("Connected...\n");
}

void disconnectCallback(const valkeyAsyncContext *c, int status) {
    if (status != VALKEY_OK) {
        printf("Error: %s\n", c->errstr);
        return;
    }
    printf("Disconnected...\n");
}

/*
 * 1- Compile this file as a shared library. Directory of "valkeymodule.h" must
 *    be in the include path.
 *       gcc -fPIC -shared -I../../valkey/src/ -I.. example-valkeymoduleapi.c -o example-valkeymoduleapi.so
 *
 * 2- Load module:
 *       valkey-server --loadmodule ./example-valkeymoduleapi.so value
 */
int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    int ret = ValkeyModule_Init(ctx, "example-valkeymoduleapi", 1, VALKEYMODULE_APIVER_1);
    if (ret != VALKEYMODULE_OK) {
        printf("error module init \n");
        return VALKEYMODULE_ERR;
    }

    valkeyAsyncContext *c = valkeyAsyncConnect("127.0.0.1", 6379);
    if (c->err) {
        /* Let *c leak for now... */
        printf("Error: %s\n", c->errstr);
        return 1;
    }

    size_t len;
    const char *val = ValkeyModule_StringPtrLen(argv[argc - 1], &len);

    ValkeyModuleCtx *module_ctx = ValkeyModule_GetDetachedThreadSafeContext(ctx);
    valkeyModuleAttach(c, module_ctx);
    valkeyAsyncSetConnectCallback(c, connectCallback);
    valkeyAsyncSetDisconnectCallback(c, disconnectCallback);
    valkeyAsyncSetTimeout(c, (struct timeval){.tv_sec = 1, .tv_usec = 0});

    /*
    In this demo, we first `set key`, then `get key` to demonstrate the basic usage of the adapter.
    Then in `getCallback`, we start a `debug sleep` command to create 1.5 second long request.
    Because we have set a 1 second timeout to the connection, the command will always fail with a
    timeout error, which is shown in the `debugCallback`.
    */

    valkeyAsyncCommand(c, NULL, NULL, "SET key %b", val, len);
    valkeyAsyncCommand(c, getCallback, (char *)"end-1", "GET key");
    return 0;
}
