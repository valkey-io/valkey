
#include "valkeymodule.h"
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <strings.h>

/* A wrap for SET command with ACL check on the key. */
int set_aclcheck_key(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 4) {
        return ValkeyModule_WrongArity(ctx);
    }

    int permissions;
    const char *flags = ValkeyModule_StringPtrLen(argv[1], NULL);

    if (!strcasecmp(flags, "W")) {
        permissions = VALKEYMODULE_CMD_KEY_UPDATE;
    } else if (!strcasecmp(flags, "R")) {
        permissions = VALKEYMODULE_CMD_KEY_ACCESS;
    } else if (!strcasecmp(flags, "*")) {
        permissions = VALKEYMODULE_CMD_KEY_UPDATE | VALKEYMODULE_CMD_KEY_ACCESS;
    } else if (!strcasecmp(flags, "~")) {
        permissions = 0; /* Requires either read or write */
    } else {
        ValkeyModule_ReplyWithError(ctx, "INVALID FLAGS");
        return VALKEYMODULE_OK;
    }

    /* Check that the key can be accessed */
    ValkeyModuleString *user_name = ValkeyModule_GetCurrentUserName(ctx);
    ValkeyModuleUser *user = ValkeyModule_GetModuleUserFromUserName(user_name);
    int ret = ValkeyModule_ACLCheckKeyPermissions(user, argv[2], permissions);
    if (ret != 0) {
        ValkeyModule_ReplyWithError(ctx, "DENIED KEY");
        ValkeyModule_FreeModuleUser(user);
        ValkeyModule_FreeString(ctx, user_name);
        return VALKEYMODULE_OK;
    }

    ValkeyModuleCallReply *rep = ValkeyModule_Call(ctx, "SET", "v", argv + 2, (size_t)argc - 2);
    if (!rep) {
        ValkeyModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }

    ValkeyModule_FreeModuleUser(user);
    ValkeyModule_FreeString(ctx, user_name);
    return VALKEYMODULE_OK;
}

/* A wrap for PUBLISH command with ACL check on the channel. */
int publish_aclcheck_channel(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) {
        return ValkeyModule_WrongArity(ctx);
    }

    /* Check that the pubsub channel can be accessed */
    ValkeyModuleString *user_name = ValkeyModule_GetCurrentUserName(ctx);
    ValkeyModuleUser *user = ValkeyModule_GetModuleUserFromUserName(user_name);
    int ret = ValkeyModule_ACLCheckChannelPermissions(user, argv[1], VALKEYMODULE_CMD_CHANNEL_SUBSCRIBE);
    if (ret != 0) {
        ValkeyModule_ReplyWithError(ctx, "DENIED CHANNEL");
        ValkeyModule_FreeModuleUser(user);
        ValkeyModule_FreeString(ctx, user_name);
        return VALKEYMODULE_OK;
    }

    ValkeyModuleCallReply *rep = ValkeyModule_Call(ctx, "PUBLISH", "v", argv + 1, (size_t)argc - 1);
    if (!rep) {
        ValkeyModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }

    ValkeyModule_FreeModuleUser(user);
    ValkeyModule_FreeString(ctx, user_name);
    return VALKEYMODULE_OK;
}


/* ACL check that validates command execution with all permissions
 * including command, keys, channels, and database access */
int aclcheck_check_permissions(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 3) {
        return ValkeyModule_WrongArity(ctx);
    }

    long long dbid;
    if (ValkeyModule_StringToLongLong(argv[1], &dbid) != VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "invalid DB index");
        return VALKEYMODULE_OK;
    }

    ValkeyModuleString *user_name = ValkeyModule_GetCurrentUserName(ctx);
    ValkeyModuleUser *user = ValkeyModule_GetModuleUserFromUserName(user_name);

    ValkeyModuleACLLogEntryReason denial_reason;
    int ret = ValkeyModule_ACLCheckPermissions(user, argv + 2, argc - 2, (int)dbid, &denial_reason);

    if (ret != VALKEYMODULE_OK) {
        int saved_errno = errno;
        if (saved_errno == EINVAL) {
            ValkeyModule_ReplyWithError(ctx, "ERR invalid arguments");
        } else if (saved_errno == EACCES) {
            ValkeyModule_ReplyWithError(ctx, "NOPERM");
            ValkeyModuleString *obj;
            switch (denial_reason) {
                case VALKEYMODULE_ACL_LOG_CMD:
                    obj = argv[2];
                    break;
                case VALKEYMODULE_ACL_LOG_KEY:
                    obj = (argc > 3) ? argv[3] : argv[2];
                    break;
                case VALKEYMODULE_ACL_LOG_CHANNEL:
                    obj = (argc > 3) ? argv[3] : argv[2];
                    break;
                case VALKEYMODULE_ACL_LOG_DB:
                    obj = argv[1];
                    break;
                default:
                    obj = argv[2];
                    break;
            }
            ValkeyModule_ACLAddLogEntry(ctx, user, obj, denial_reason);
        } else {
            ValkeyModule_ReplyWithError(ctx, "ERR unexpected error");
        }
        ValkeyModule_FreeModuleUser(user);
        ValkeyModule_FreeString(ctx, user_name);
        return VALKEYMODULE_OK;
    }

    ValkeyModuleCallReply *rep = ValkeyModule_Call(ctx, ValkeyModule_StringPtrLen(argv[2], NULL), "v", argv + 3, (size_t)argc - 3);
    if (!rep) {
        ValkeyModule_ReplyWithError(ctx, "NULL reply");
    } else {
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }

    ValkeyModule_FreeModuleUser(user);
    ValkeyModule_FreeString(ctx, user_name);
    return VALKEYMODULE_OK;
}

/* A wrap for RM_Call that check first that the command can be executed */
int rm_call_aclcheck_cmd(ValkeyModuleCtx *ctx, ValkeyModuleUser *user, ValkeyModuleString **argv, int argc) {
    if (argc < 2) {
        return ValkeyModule_WrongArity(ctx);
    }

    /* Check that the command can be executed */
    int ret = ValkeyModule_ACLCheckCommandPermissions(user, argv + 1, argc - 1);
    if (ret != 0) {
        ValkeyModule_ReplyWithError(ctx, "DENIED CMD");
        /* Add entry to ACL log */
        ValkeyModule_ACLAddLogEntry(ctx, user, argv[1], VALKEYMODULE_ACL_LOG_CMD);
        return VALKEYMODULE_OK;
    }

    const char* cmd = ValkeyModule_StringPtrLen(argv[1], NULL);

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, cmd, "v", argv + 2, (size_t)argc - 2);
    if(!rep){
        ValkeyModule_ReplyWithError(ctx, "NULL reply returned");
    }else{
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }

    return VALKEYMODULE_OK;
}

int rm_call_aclcheck_cmd_default_user(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    ValkeyModuleString *user_name = ValkeyModule_GetCurrentUserName(ctx);
    ValkeyModuleUser *user = ValkeyModule_GetModuleUserFromUserName(user_name);

    int res = rm_call_aclcheck_cmd(ctx, user, argv, argc);

    ValkeyModule_FreeModuleUser(user);
    ValkeyModule_FreeString(ctx, user_name);
    return res;
}

int rm_call_aclcheck_cmd_module_user(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    /* Create a user and authenticate */
    ValkeyModuleUser *user = ValkeyModule_CreateModuleUser("testuser1");
    ValkeyModule_SetModuleUserACL(user, "allcommands");
    ValkeyModule_SetModuleUserACL(user, "allkeys");
    ValkeyModule_SetModuleUserACL(user, "on");
    ValkeyModule_AuthenticateClientWithUser(ctx, user, NULL, NULL, NULL);

    int res = rm_call_aclcheck_cmd(ctx, user, argv, argc);

    /* authenticated back to "default" user (so once we free testuser1 we will not disconnected */
    ValkeyModule_AuthenticateClientWithACLUser(ctx, "default", 7, NULL, NULL, NULL);
    ValkeyModule_FreeModuleUser(user);
    return res;
}

int rm_call_aclcheck_with_errors(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc){
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if(argc < 2){
        return ValkeyModule_WrongArity(ctx);
    }

    const char* cmd = ValkeyModule_StringPtrLen(argv[1], NULL);

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, cmd, "vEC", argv + 2, (size_t)argc - 2);
    ValkeyModule_ReplyWithCallReply(ctx, rep);
    ValkeyModule_FreeCallReply(rep);
    return VALKEYMODULE_OK;
}

/* A wrap for RM_Call that pass the 'C' flag to do ACL check on the command. */
int rm_call_aclcheck(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc){
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if(argc < 2){
        return ValkeyModule_WrongArity(ctx);
    }

    const char* cmd = ValkeyModule_StringPtrLen(argv[1], NULL);

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, cmd, "vC", argv + 2, (size_t)argc - 2);
    if(!rep) {
        char err[100];
        switch (errno) {
            case EACCES:
                ValkeyModule_ReplyWithError(ctx, "ERR NOPERM");
                break;
            default:
                snprintf(err, sizeof(err) - 1, "ERR errno=%d", errno);
                ValkeyModule_ReplyWithError(ctx, err);
                break;
        }
    } else {
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }

    return VALKEYMODULE_OK;
}

int module_check_key_permission(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if(argc != 3){
        return ValkeyModule_WrongArity(ctx);
    }

    size_t key_len = 0;
    unsigned int flags = 0;
    const char* key_prefix = ValkeyModule_StringPtrLen(argv[1], &key_len);
    const char* check_what = ValkeyModule_StringPtrLen(argv[2], NULL);
    if (strcasecmp(check_what, "R") == 0) {
        flags = VALKEYMODULE_CMD_KEY_ACCESS;
    } else if(strcasecmp(check_what, "W") == 0) {
        flags = VALKEYMODULE_CMD_KEY_INSERT | VALKEYMODULE_CMD_KEY_DELETE | VALKEYMODULE_CMD_KEY_UPDATE;
    } else {
        ValkeyModule_ReplyWithSimpleString(ctx, "EINVALID");
        return VALKEYMODULE_OK;
    }

    ValkeyModuleString* user_name = ValkeyModule_GetCurrentUserName(ctx);
    ValkeyModuleUser *user = ValkeyModule_GetModuleUserFromUserName(user_name);
    int rc = ValkeyModule_ACLCheckKeyPrefixPermissions(user, key_prefix, key_len, flags);
    if (rc == VALKEYMODULE_OK) {
        // Access granted.
        ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        // Access denied.
        ValkeyModule_ReplyWithSimpleString(ctx, "EACCESS");
    }
    ValkeyModule_FreeModuleUser(user);
    ValkeyModule_FreeString(ctx, user_name);
    return VALKEYMODULE_OK;
}

int module_test_acl_category(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

int commandBlockCheck(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    int response_ok = 0;
    int result = ValkeyModule_CreateCommand(ctx,"command.that.should.fail", module_test_acl_category, "", 0, 0, 0);
    response_ok |= (result == VALKEYMODULE_OK);

    result = ValkeyModule_AddACLCategory(ctx,"blockedcategory");
    response_ok |= (result == VALKEYMODULE_OK);
    
    ValkeyModuleCommand *parent = ValkeyModule_GetCommand(ctx,"block.commands.outside.onload");
    result = ValkeyModule_SetCommandACLCategories(parent, "write");
    response_ok |= (result == VALKEYMODULE_OK);

    result = ValkeyModule_CreateSubcommand(parent,"subcommand.that.should.fail",module_test_acl_category,"",0,0,0);
    response_ok |= (result == VALKEYMODULE_OK);
    
    /* This validates that it's not possible to create commands or add
     * a new ACL Category outside OnLoad function.
     * thus returns an error if they succeed. */
    if (response_ok) {
        ValkeyModule_ReplyWithError(ctx, "UNEXPECTEDOK");
    } else {
        ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    }
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {

    if (ValkeyModule_Init(ctx,"aclcheck",1,VALKEYMODULE_APIVER_1)== VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (argc > 1) return ValkeyModule_WrongArity(ctx);
    
    /* When that flag is passed, we try to create too many categories,
     * and the test expects this to fail. In this case the server returns VALKEYMODULE_ERR
     * and set errno to ENOMEM*/
    if (argc == 1) {
        long long fail_flag = 0;
        ValkeyModule_StringToLongLong(argv[0], &fail_flag);
        if (fail_flag) {
            for (size_t j = 0; j < 45; j++) {
                ValkeyModuleString* name =  ValkeyModule_CreateStringPrintf(ctx, "customcategory%zu", j);
                if (ValkeyModule_AddACLCategory(ctx, ValkeyModule_StringPtrLen(name, NULL)) == VALKEYMODULE_ERR) {
                    ValkeyModule_Assert(errno == ENOMEM);
                    ValkeyModule_FreeString(ctx, name);
                    return VALKEYMODULE_ERR;
                }
                ValkeyModule_FreeString(ctx, name);
            }
        }
    }

    if (ValkeyModule_CreateCommand(ctx,"aclcheck.set.check.key", set_aclcheck_key,"write",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"block.commands.outside.onload", commandBlockCheck,"write",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"aclcheck.module.command.aclcategories.write", module_test_acl_category,"write",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    ValkeyModuleCommand *aclcategories_write = ValkeyModule_GetCommand(ctx,"aclcheck.module.command.aclcategories.write");

    if (ValkeyModule_SetCommandACLCategories(aclcategories_write, "write") == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"aclcheck.module.command.aclcategories.write.function.read.category", module_test_acl_category,"write",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    ValkeyModuleCommand *read_category = ValkeyModule_GetCommand(ctx,"aclcheck.module.command.aclcategories.write.function.read.category");

    if (ValkeyModule_SetCommandACLCategories(read_category, "read") == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"aclcheck.module.command.aclcategories.read.only.category", module_test_acl_category,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    ValkeyModuleCommand *read_only_category = ValkeyModule_GetCommand(ctx,"aclcheck.module.command.aclcategories.read.only.category");

    if (ValkeyModule_SetCommandACLCategories(read_only_category, "read") == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"aclcheck.publish.check.channel", publish_aclcheck_channel,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"aclcheck.check.permissions", aclcheck_check_permissions,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"aclcheck.rm_call.check.cmd", rm_call_aclcheck_cmd_default_user,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"aclcheck.rm_call.check.cmd.module.user", rm_call_aclcheck_cmd_module_user,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"aclcheck.rm_call", rm_call_aclcheck,
                                  "write",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"aclcheck.rm_call_with_errors", rm_call_aclcheck_with_errors,
                                      "write",0,0,0) == VALKEYMODULE_ERR)
            return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"acl.check_key_prefix", 
                                   module_check_key_permission, 
                                   "", 
                                   0, 
                                   0, 
                                   0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    } 
    /* This validates that, when module tries to add a category with invalid characters,
     * the server returns VALKEYMODULE_ERR and set errno to `EINVAL` */
    if (ValkeyModule_AddACLCategory(ctx,"!nval!dch@r@cter$") == VALKEYMODULE_ERR)
        ValkeyModule_Assert(errno == EINVAL);
    else 
        return VALKEYMODULE_ERR;
    
    /* This validates that, when module tries to add a category that already exists,
     * the server returns VALKEYMODULE_ERR and set errno to `EBUSY` */
    if (ValkeyModule_AddACLCategory(ctx,"write") == VALKEYMODULE_ERR)
        ValkeyModule_Assert(errno == EBUSY);
    else 
        return VALKEYMODULE_ERR;
    
    if (ValkeyModule_AddACLCategory(ctx,"foocategory") == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    
    if (ValkeyModule_CreateCommand(ctx,"aclcheck.module.command.test.add.new.aclcategories", module_test_acl_category,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    ValkeyModuleCommand *test_add_new_aclcategories = ValkeyModule_GetCommand(ctx,"aclcheck.module.command.test.add.new.aclcategories");

    if (ValkeyModule_SetCommandACLCategories(test_add_new_aclcategories, "foocategory") == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    
    return VALKEYMODULE_OK;
}
