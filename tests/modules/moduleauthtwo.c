#include "valkeymodule.h"

#include <string.h>

/* This is a second sample module to validate that module authentication callbacks can be registered
 * from multiple modules. */

/* Non Blocking Module Auth callback / implementation. */
int auth_cb(ValkeyModuleCtx *ctx, ValkeyModuleString *username, ValkeyModuleString *password, ValkeyModuleString **err) {
    const char *user = ValkeyModule_StringPtrLen(username, NULL);
    const char *pwd = ValkeyModule_StringPtrLen(password, NULL);
    if (!strcmp(user,"foo") && !strcmp(pwd,"allow_two")) {
        ValkeyModule_AuthenticateClientWithACLUser(ctx, "foo", 3, NULL, NULL, NULL);
        return VALKEYMODULE_AUTH_HANDLED;
    }
    else if (!strcmp(user,"foo") && !strcmp(pwd,"deny_two")) {
        ValkeyModuleString *log = ValkeyModule_CreateString(ctx, "Module Auth", 11);
        ValkeyModule_ACLAddLogEntryByUserName(ctx, username, log, VALKEYMODULE_ACL_LOG_AUTH);
        ValkeyModule_FreeString(ctx, log);
        const char *err_msg = "Auth denied by Misc Module.";
        *err = ValkeyModule_CreateString(ctx, err_msg, strlen(err_msg));
        return VALKEYMODULE_AUTH_HANDLED;
    }
    return VALKEYMODULE_AUTH_NOT_HANDLED;
}

int test_rm_register_auth_cb(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    ValkeyModule_RegisterAuthCallback(ctx, auth_cb);
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx,"moduleauthtwo",1,VALKEYMODULE_APIVER_1)== VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"testmoduletwo.rm_register_auth_cb", test_rm_register_auth_cb,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    return VALKEYMODULE_OK;
}