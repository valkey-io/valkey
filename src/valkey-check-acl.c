/* valkey-check-acl.c -- Offline ACL file validator.
 *
 * Validates ACL configuration files (aclfile or valkey.conf user directives)
 * without starting a server. Reuses the server's actual ACL parsing code
 * so validation results are guaranteed to match server behavior.
 *
 * This runs as a multi-call binary dispatched from valkey-server's main()
 * after initServerConfig() and ACLInit(), so the full command table and
 * ACL subsystem are already initialized.
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Defined in server.c, needed for pubsub_patterns initialization. */
extern dictType objToHashtableDictType;

/* ---------------------------------------------------------------------------
 * Error collection
 * --------------------------------------------------------------------------- */

/* We use Valkey's built-in list for error/warning collection.
 * Each node value is an sds string. */
static void aclCheckerListFree(list *l) {
    listIter li;
    listNode *ln;
    listRewind(l, &li);
    while ((ln = listNext(&li)) != NULL) {
        sdsfree(listNodeValue(ln));
    }
    listRelease(l);
}

/* ---------------------------------------------------------------------------
 * Validation levels and options
 * --------------------------------------------------------------------------- */

#define CHECK_ACL_LEVEL_SYNTAX 0
#define CHECK_ACL_LEVEL_SEMANTIC 1
#define CHECK_ACL_LEVEL_FULL 2

typedef struct {
    int major;
    int minor;
    int patch; /* 0 if not specified */
} aclCheckerVersion;

/* Returns 1 if v >= o. */
static inline int versionGE(const aclCheckerVersion *v, const aclCheckerVersion *o) {
    if (v->major != o->major) {
        return v->major > o->major;
    }
    if (v->minor != o->minor) {
        return v->minor > o->minor;
    }
    return v->patch >= o->patch;
}

typedef struct {
    int level;
    int json;
    int verbose;
    int quiet;
    int fail_fast;
    int ignore_unknown_commands;
    int simplify;
    const char *filename; /* NULL means stdin */
    aclCheckerVersion version;
    int version_set;                /* 1 if --version was provided */
    const char *commands_files[16]; /* --commands-file paths (max 16) */
    int num_commands_files;
} aclCheckerConfig;

/* Parse a version string like "7.0" or "7.2.1". Returns 0 on success, -1 on error. */
static int parseVersion(const char *str, aclCheckerVersion *v) {
    v->patch = 0;
    int n = sscanf(str, "%d.%d.%d", &v->major, &v->minor, &v->patch);
    if (n < 2) {
        return -1;
    }
    if (v->major < 0 || v->minor < 0 || v->patch < 0) {
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * External commands file support
 *
 * Line-based format:
 *   command_name since_version category1,category2,...
 * Lines starting with # are comments. Empty lines are ignored.
 * --------------------------------------------------------------------------- */

/* Dict of external commands: key = sds command name, value = aclCheckerVersion* (heap-allocated). */

static void extCommandsEntryDestructor(void *entry) {
    dictEntry *de = entry;
    sdsfree(dictGetKey(de));
    zfree(dictGetVal(de));
    zfree(de);
}

static dictType extCommandsDictType = {
    .entryGetKey = dictEntryGetKey,
    .hashFunction = dictSdsCaseHash,
    .keyCompare = dictSdsKeyCaseCompare,
    .entryDestructor = extCommandsEntryDestructor,
};

static void extCommandsAdd(dict *ext, sds name, aclCheckerVersion since) {
    aclCheckerVersion *v = zmalloc(sizeof(*v));
    *v = since;
    dictReplace(ext, name, v);
}

static aclCheckerVersion *extCommandsLookup(dict *ext, const char *name, size_t len) {
    sds key = sdsnewlen(name, len);
    dictEntry *de = dictFind(ext, key);
    sdsfree(key);
    if (de) {
        return dictGetVal(de);
    }
    return NULL;
}

/* Load a commands file. Returns 0 on success, -1 on error.
 * Expected format: one command per line as "command_name [since_version]".
 * Lines starting with '#' are comments. Blank lines are ignored.
 * Example:
 *   mymodule.cmd 7.2.0
 *   mymodule.other
 */
static int loadCommandsFile(const char *path, dict *ext) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "Error opening commands file '%s': %s\n", path, strerror(errno));
        return -1;
    }

    char buf[4096];
    int linenum = 0;
    while (fgets(buf, sizeof(buf), fp)) {
        linenum++;
        /* Trim and skip comments/empty lines. */
        char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') {
            continue;
        }

        /* Parse: name since categories */
        int argc;
        sds *argv = sdssplitargs(p, &argc);
        if (!argv || argc < 2) {
            fprintf(stderr, "%s:%d: Invalid format (expected: name since [categories])\n", path, linenum);
            if (argv) {
                sdsfreesplitres(argv, argc);
            }
            fclose(fp);
            return -1;
        }

        aclCheckerVersion since;
        if (parseVersion(argv[1], &since) != 0) {
            fprintf(stderr, "%s:%d: Invalid version '%s'\n", path, linenum, argv[1]);
            sdsfreesplitres(argv, argc);
            fclose(fp);
            return -1;
        }

        sds name = sdsdup(argv[0]);
        sdstolower(name);
        extCommandsAdd(ext, name, since);
        sdsfreesplitres(argv, argc);
    }

    fclose(fp);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Version-gated token validation
 *
 * Pre-checks ACL tokens against the requested version BEFORE passing them
 * to ACLStringSetUser. Returns NULL if the token is valid for the version,
 * or an error string (caller must sdsfree) if it's not.
 * --------------------------------------------------------------------------- */

static sds checkTokenVersion(const char *op, size_t oplen, const aclCheckerVersion *ver) {
    static const aclCheckerVersion v6_2 = {6, 2, 0};
    static const aclCheckerVersion v7_0 = {7, 0, 0};
    static const aclCheckerVersion v9_1 = {9, 1, 0};

    /* Channels: introduced in 6.2 */
    if (!versionGE(ver, &v6_2)) {
        if (op[0] == '&')
            return sdscatprintf(sdsempty(), "Channel pattern '&' requires version >= 6.2");
        if (!strcasecmp(op, "allchannels"))
            return sdscatprintf(sdsempty(), "'allchannels' requires version >= 6.2");
        if (!strcasecmp(op, "resetchannels"))
            return sdscatprintf(sdsempty(), "'resetchannels' requires version >= 6.2");
    }

    /* Selectors, key permissions, subcommand deny, sanitize-payload: introduced in 7.0 */
    if (!versionGE(ver, &v7_0)) {
        /* Selector tokens: sdssplitargs splits on spaces inside parens, so
         * we see tokens starting with '(' or ending with ')'. */
        if (op[0] == '(')
            return sdscatprintf(sdsempty(), "Selectors '(...)' require version >= 7.0");
        if (oplen > 0 && op[oplen - 1] == ')')
            return sdscatprintf(sdsempty(), "Selectors '(...)' require version >= 7.0");
        if (!strcasecmp(op, "clearselectors"))
            return sdscatprintf(sdsempty(), "'clearselectors' requires version >= 7.0");
        if (op[0] == '%')
            return sdscatprintf(sdsempty(), "Key permission '%%R~/%%W~/%%RW~' requires version >= 7.0");
        if (!strcasecmp(op, "sanitize-payload"))
            return sdscatprintf(sdsempty(), "'sanitize-payload' requires version >= 7.0");
        if (!strcasecmp(op, "skip-sanitize-payload"))
            return sdscatprintf(sdsempty(), "'skip-sanitize-payload' requires version >= 7.0");
        /* Deny subcommand: -cmd|sub */
        if (op[0] == '-' && op[1] != '@' && strchr(op, '|'))
            return sdscatprintf(sdsempty(), "Denying subcommands '-cmd|sub' requires version >= 7.0");
    }

    /* Database ACL: introduced in 9.1 (Valkey) */
    if (!versionGE(ver, &v9_1)) {
        if (!strcasecmp(op, "alldbs"))
            return sdscatprintf(sdsempty(), "'alldbs' requires version >= 9.1");
        if (!strcasecmp(op, "resetdbs"))
            return sdscatprintf(sdsempty(), "'resetdbs' requires version >= 9.1");
        if (strncasecmp(op, "db=", 3) == 0)
            return sdscatprintf(sdsempty(), "'db=' requires version >= 9.1");
    }

    return NULL; /* Token is valid for this version. */
}

/* ---------------------------------------------------------------------------
 * Core validation: validate a single user line
 *
 * Takes a line like: user alice on >pass ~* +@all
 * argv[0] = "user", argv[1] = username, argv[2..] = rules
 * --------------------------------------------------------------------------- */

/* Validate a single user line. Returns the validated user on success (caller
 * must free with ACLFreeUser), or NULL on error. */
static user *validateUserLine(sds *argv, int argc, int linenum, const char *filename, aclCheckerConfig *config, dict *ext_commands, list *errors) {
    if (argc < 2 || strcasecmp(argv[0], "user")) {
        listAddNodeTail(errors,
                        sdscatprintf(sdsempty(), "%s:%d: Line should start with 'user' keyword followed by the username.",
                                     filename, linenum));
        return NULL;
    }

    /* Check for spaces in username. */
    for (size_t k = 0; k < sdslen(argv[1]); k++) {
        if (argv[1][k] == ' ') {
            listAddNodeTail(errors,
                            sdscatprintf(sdsempty(), "%s:%d: Username '%s' contains invalid characters.",
                                         filename, linenum, argv[1]));
            return NULL;
        }
    }

    /* Pre-validation: version-gated syntax check + command version/existence check.
     * We do both in a single pass over the tokens. */
    for (int i = 2; i < argc; i++) {
        const char *op = argv[i];
        size_t oplen = sdslen(argv[i]);

        /* Version syntax gating (must happen before ACLStringSetUser). */
        if (config->version_set) {
            sds err = checkTokenVersion(op, oplen, &config->version);
            if (err) {
                listAddNodeTail(errors,
                                sdscatprintf(sdsempty(), "%s:%d: %s", filename, linenum, err));
                sdsfree(err);
                return NULL;
            }
        }
    }

    /* Validate rules using ACLStringSetUser on a fresh unlinked user.
     * Note: ACLStringSetUser rejects unknown commands (ENOENT) unlike
     * ACLAppendUserForLoading which skips them. */
    user *fakeuser = ACLCreateUnlinkedUser();
    sds err = ACLStringSetUser(fakeuser, argv[1], argv + 2, argc - 2);
    if (err) {
        /* Suppress unknown command errors (not category errors) when:
         * - --ignore-unknown-commands is set
         * - --commands-file is loaded (command might be in the file)
         * - --level syntax (only check syntax, not command existence)
         * Category errors (+@bogus) are never suppressed. */
        int is_unknown = strstr(err, "Unknown command or category name in ACL") != NULL;
        int is_category = strstr(err, "modifier '+@") || strstr(err, "modifier '-@");
        if (is_unknown && !is_category &&
            (config->ignore_unknown_commands || dictSize(ext_commands) > 0 ||
             config->level == CHECK_ACL_LEVEL_SYNTAX)) {
            sdsfree(err);
        } else {
            listAddNodeTail(errors,
                            sdscatprintf(sdsempty(), "%s:%d: %s", filename, linenum, err));
            sdsfree(err);
            ACLFreeUser(fakeuser);
            return NULL;
        }
    }

    /* Post-validation: check command versions and unknown commands.
     * Skip when --level syntax (only checking syntax, not command existence). */
    if (config->level > CHECK_ACL_LEVEL_SYNTAX &&
        (config->version_set || dictSize(ext_commands) > 0 || !config->ignore_unknown_commands)) {
        for (int i = 2; i < argc; i++) {
            const char *op = argv[i];
            size_t oplen = sdslen(argv[i]);

            /* Strip selector parens. */
            if (op[0] == '(') {
                op++;
                oplen--;
            }
            if (oplen > 0 && op[oplen - 1] == ')') {
                oplen--;
            }
            if (oplen == 0) {
                continue;
            }

            /* Only check +cmd/-cmd tokens. */
            if (op[0] != '+' && op[0] != '-') {
                continue;
            }
            if (oplen == 1) {
                continue;
            }
            if (op[1] == '@') {
                continue;
            }

            const char *cmdname = op + 1;
            size_t cmdlen = oplen - 1;
            if (cmdlen == 0) {
                continue;
            }

            sds lookup = sdsnewlen(cmdname, cmdlen);
            struct serverCommand *cmd = lookupCommandBySds(lookup);
            sdsfree(lookup);

            if (cmd == NULL) {
                /* Check external commands files. */
                aclCheckerVersion *ext_ver = extCommandsLookup(ext_commands, cmdname, cmdlen);
                if (ext_ver) {
                    if (config->version_set &&
                        !versionGE(&config->version, ext_ver)) {
                        listAddNodeTail(errors,
                                        sdscatprintf(sdsempty(), "%s:%d: Command '%.*s' requires version >= %d.%d",
                                                     filename, linenum, (int)cmdlen, cmdname,
                                                     ext_ver->major, ext_ver->minor));
                        ACLFreeUser(fakeuser);
                        return NULL;
                    }
                } else if (!config->ignore_unknown_commands) {
                    listAddNodeTail(errors,
                                    sdscatprintf(sdsempty(), "%s:%d: Unknown command '%.*s'",
                                                 filename, linenum, (int)cmdlen, cmdname));
                    ACLFreeUser(fakeuser);
                    return NULL;
                }
            } else if (config->version_set && cmd->since) {
                aclCheckerVersion cmd_ver;
                if (parseVersion(cmd->since, &cmd_ver) == 0 &&
                    !versionGE(&config->version, &cmd_ver)) {
                    listAddNodeTail(errors,
                                    sdscatprintf(sdsempty(), "%s:%d: Command '%s' requires version >= %d.%d (introduced in %s)",
                                                 filename, linenum, cmd->fullname, cmd_ver.major, cmd_ver.minor, cmd->since));
                    ACLFreeUser(fakeuser);
                    return NULL;
                }
            }
        }
    }

    return fakeuser;
}

/* ---------------------------------------------------------------------------
 * Command rules simplification
 *
 * ACLDescribeUser outputs command_rules as-is, which may contain redundancies
 * like "+@all +get" (get already allowed by +@all) or "-@all -get" (get
 * already denied). This function removes such redundancies.
 * --------------------------------------------------------------------------- */

static sds simplifyCommandRules(sds rules) {
    int argc;
    sds *argv = sdssplitargs(rules, &argc);
    if (!argv || argc == 0) {
        return sdsdup(rules);
    }

    /* First token should be +@all or -@all. */
    int base_allow = !strcasecmp(argv[0], "+@all");

    /* Collect category flags that are in the override set. */
    uint64_t added_categories = 0;
    uint64_t removed_categories = 0;
    for (int i = 1; i < argc; i++) {
        if (sdslen(argv[i]) > 2 && argv[i][1] == '@') {
            uint64_t cflag = ACLGetCommandCategoryFlagByName(argv[i] + 2);
            if (cflag) {
                if (argv[i][0] == '+')
                    added_categories |= cflag;
                else
                    removed_categories |= cflag;
            }
        }
    }

    /* Build simplified output. */
    sds result = sdsdup(argv[0]);

    for (int i = 1; i < argc; i++) {
        if (sdslen(argv[i]) < 2) {
            continue;
        }
        char sign = argv[i][0];
        int is_additive = (sign == '+');

        if (argv[i][1] == '@') {
            /* Category rule — keep it (categories are already meaningful overrides). */
            result = sdscatlen(result, " ", 1);
            result = sdscatsds(result, argv[i]);
        } else {
            /* Command rule — check if redundant. */
            sds cmdname = sdsnewlen(argv[i] + 1, sdslen(argv[i]) - 1);
            struct serverCommand *cmd = lookupCommandBySds(cmdname);
            sdsfree(cmdname);

            if (is_additive == base_allow) {
                /* Same direction as base — redundant unless an opposing category
                 * removed/added it back. */
                if (cmd) {
                    if (base_allow && (cmd->acl_categories & removed_categories)) {
                        result = sdscatlen(result, " ", 1);
                        result = sdscatsds(result, argv[i]);
                    } else if (!base_allow && (cmd->acl_categories & added_categories)) {
                        result = sdscatlen(result, " ", 1);
                        result = sdscatsds(result, argv[i]);
                    }
                    /* Otherwise: redundant with base, skip. */
                } else {
                    /* Unknown command (module?) — keep it to be safe. */
                    result = sdscatlen(result, " ", 1);
                    result = sdscatsds(result, argv[i]);
                }
            } else {
                /* Opposite direction from base — check if a category already covers it. */
                if (cmd) {
                    if (is_additive && (cmd->acl_categories & added_categories)) {
                        continue;
                    }
                    if (!is_additive && (cmd->acl_categories & removed_categories)) {
                        continue;
                    }
                }
                /* Not redundant — keep. */
                result = sdscatlen(result, " ", 1);
                result = sdscatsds(result, argv[i]);
            }
        }
    }

    sdsfreesplitres(argv, argc);
    return result;
}

/* Simplify the full ACL description by post-processing command rules. */
static sds simplifyAclDescription(sds descr) {
    /* Find the command rules section: starts with "+@all " or "-@all ". */
    char *cmd_start = strstr(descr, "+@all");
    if (!cmd_start) {
        cmd_start = strstr(descr, "-@all");
    }
    if (!cmd_start) {
        return sdsdup(descr); /* No command rules found. */
    }

    /* Everything before the command rules. */
    size_t prefix_len = cmd_start - descr;
    sds prefix = sdsnewlen(descr, prefix_len);

    /* The command rules (from +@all/-@all to end, or to next selector). */
    char *selector_start = strstr(cmd_start, " (");
    sds cmd_rules;
    sds suffix = sdsempty();
    if (selector_start) {
        cmd_rules = sdsnewlen(cmd_start, selector_start - cmd_start);
        /* Process selectors: simplify command rules inside each one. */
        char *p = selector_start;
        while (p && *p) {
            /* Find the selector content between ( and ). */
            char *open = strchr(p, '(');
            if (!open) {
                suffix = sdscat(suffix, p);
                break;
            }
            /* Copy text before the '(' */
            suffix = sdscatlen(suffix, p, open - p);
            char *close = strchr(open, ')');
            if (!close) {
                suffix = sdscat(suffix, open);
                break;
            }
            /* Extract inner content and simplify it. */
            sds inner = sdsnewlen(open + 1, close - open - 1);
            /* Find command rules inside selector. */
            char *inner_cmd = strstr(inner, "+@all");
            if (!inner_cmd) {
                inner_cmd = strstr(inner, "-@all");
            }
            if (inner_cmd) {
                size_t inner_prefix_len = inner_cmd - inner;
                sds inner_prefix = sdsnewlen(inner, inner_prefix_len);
                sds inner_cmds = sdsnew(inner_cmd);
                sds simplified_inner = simplifyCommandRules(inner_cmds);
                suffix = sdscatfmt(suffix, "(%S%S)", inner_prefix, simplified_inner);
                sdsfree(inner_prefix);
                sdsfree(inner_cmds);
                sdsfree(simplified_inner);
            } else {
                suffix = sdscatfmt(suffix, "(%S)", inner);
            }
            sdsfree(inner);
            p = close + 1;
        }
    } else {
        cmd_rules = sdsnew(cmd_start);
    }

    sds simplified_cmds = simplifyCommandRules(cmd_rules);
    sdsfree(cmd_rules);

    sds result = sdscatfmt(sdsempty(), "%S%S%S", prefix, simplified_cmds, suffix);
    sdsfree(prefix);
    sdsfree(simplified_cmds);
    sdsfree(suffix);
    return result;
}

/* ---------------------------------------------------------------------------
 * File validation: read and validate an ACL file or valkey.conf
 * --------------------------------------------------------------------------- */

static sds readFileContent(const char *filename) {
    FILE *fp;
    if (filename == NULL || !strcmp(filename, "-")) {
        fp = stdin;
    } else {
        fp = fopen(filename, "r");
        if (!fp) {
            return NULL;
        }
    }

    sds content = sdsempty();
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        content = sdscat(content, buf);
    }
    if (fp != stdin) {
        fclose(fp);
    }
    return content;
}

/* Detect if content is a valkey.conf (has both user directives AND
 * non-user config directives) vs a pure ACL file. */
static int detectIsConfig(sds content) {
    int totlines;
    sds *lines = sdssplitlen(content, sdslen(content), "\n", 1, &totlines);
    int has_user_lines = 0;
    int has_other_lines = 0;

    for (int i = 0; i < totlines; i++) {
        lines[i] = sdstrim(lines[i], " \t\r\n");
        if (lines[i][0] == '\0' || lines[i][0] == '#') {
            continue;
        }
        if (strncasecmp(lines[i], "user ", 5) == 0 || strncasecmp(lines[i], "user\t", 5) == 0)
            has_user_lines = 1;
        else
            has_other_lines = 1;
    }
    sdsfreesplitres(lines, totlines);
    return has_user_lines && has_other_lines;
}

/* Validate ACL content. Returns: 0 = no errors, 1 = syntax errors, 2 = semantic errors. */
static int validateContent(sds content, const char *display_name, aclCheckerConfig *config, dict *ext_commands, list *errors, list *warnings, sds *simplified_out) {
    int totlines;
    sds *lines = sdssplitlen(content, sdslen(content), "\n", 1, &totlines);
    int is_config = detectIsConfig(content);
    int user_count = 0;
    int has_default = 0;
    int error_count = 0;
    int has_semantic_error = 0; /* For exit code 2 vs 1 */

    /* Track seen usernames for duplicate detection. */
    dict *seen_users = dictCreate(&stringSetDictType);

    for (int i = 0; i < totlines; i++) {
        int linenum = i + 1;
        lines[i] = sdstrim(lines[i], " \t\r\n");

        /* Skip blank lines and comments. */
        if (lines[i][0] == '\0' || lines[i][0] == '#') {
            continue;
        }

        /* In config mode, skip non-user lines. */
        if (is_config && strncasecmp(lines[i], "user ", 5) != 0 && strncasecmp(lines[i], "user\t", 5) != 0) {
            continue;
        }

        /* Split line into arguments. */
        int argc;
        sds *argv = sdssplitargs(lines[i], &argc);
        if (!argv) {
            listAddNodeTail(errors,
                            sdscatprintf(sdsempty(), "%s:%d: Unbalanced quotes in ACL line.",
                                         display_name, linenum));
            error_count++;
            if (config->fail_fast) {
                break;
            }
            continue;
        }
        if (argc == 0) {
            sdsfreesplitres(argv, argc);
            continue;
        }

        /* Duplicate user detection (matches Valkey behavior: reject duplicates). */
        if (argc >= 2 && !strcasecmp(argv[0], "user")) {
            if (dictFind(seen_users, argv[1])) {
                listAddNodeTail(errors,
                                sdscatprintf(sdsempty(), "%s:%d: Duplicate user '%s'. A user can only be defined once.",
                                             display_name, linenum, argv[1]));
                has_semantic_error = 1;
                error_count++;
                sdsfreesplitres(argv, argc);
                if (config->fail_fast) {
                    break;
                }
                continue;
            }
            /* Track this username. */
            dictAdd(seen_users, sdsdup(argv[1]), NULL);
        }

        user *validated_user = validateUserLine(argv, argc, linenum, display_name, config, ext_commands, errors);
        if (!validated_user) {
            /* Errors from version gating or unknown commands are semantic (exit 2).
             * We detect this by checking if the error message mentions version or unknown. */
            if (listLength(errors) > 0) {
                sds last_err = listNodeValue(listLast(errors));
                if (strstr(last_err, "requires version") || strstr(last_err, "Unknown command"))
                    has_semantic_error = 1;
            }
            error_count++;
            if (config->fail_fast) {
                sdsfreesplitres(argv, argc);
                break;
            }
        } else {
            user_count++;
            if (!strcasecmp(argv[1], "default")) {
                has_default = 1;
            }

            /* Collect simplified output if --simplify is set. */
            if (config->simplify && simplified_out) {
                robj *descr = ACLDescribeUser(validated_user);
                sds simplified = simplifyAclDescription(objectGetVal(descr));
                *simplified_out = sdscatprintf(*simplified_out, "user %s %s\n",
                                               argv[1], simplified);
                sdsfree(simplified);
                decrRefCount(descr);
            }

            /* Completeness warnings (--level full). */
            if (config->level >= CHECK_ACL_LEVEL_FULL) {
                /* Warn: user is enabled with no passwords and not nopass. */
                if ((validated_user->flags & USER_FLAG_ENABLED) &&
                    !(validated_user->flags & USER_FLAG_NOPASS) &&
                    listLength(validated_user->passwords) == 0) {
                    listAddNodeTail(warnings,
                                    sdscatprintf(sdsempty(),
                                                 "%s:%d: User '%s' is enabled but has no passwords and is not set to 'nopass'.",
                                                 display_name, linenum, argv[1]));
                }

                /* Warn: non-default user with overly permissive rules.
                 * Check the raw rule tokens for ~* and +@all. */
                if (strcasecmp(argv[1], "default")) {
                    int has_allkeys = 0, has_allcommands = 0;
                    for (int r = 2; r < argc; r++) {
                        if (!strcasecmp(argv[r], "~*") || !strcasecmp(argv[r], "allkeys"))
                            has_allkeys = 1;
                        if (!strcasecmp(argv[r], "+@all") || !strcasecmp(argv[r], "allcommands"))
                            has_allcommands = 1;
                    }
                    if (has_allkeys && has_allcommands) {
                        listAddNodeTail(warnings,
                                        sdscatprintf(sdsempty(),
                                                     "%s:%d: User '%s' has full access (~* +@all).",
                                                     display_name, linenum, argv[1]));
                    }
                }
            }

            ACLFreeUser(validated_user);
        }

        sdsfreesplitres(argv, argc);
    }

    /* Completeness: missing default user. */
    if (config->level >= CHECK_ACL_LEVEL_FULL && !has_default && error_count == 0) {
        listAddNodeTail(warnings,
                        sdscatprintf(sdsempty(), "%s: No 'default' user defined.", display_name));
    }

    dictRelease(seen_users);
    sdsfreesplitres(lines, totlines);

    if (config->verbose && error_count == 0)
        fprintf(stderr, "Validated %d user(s).\n", user_count);

    if (error_count == 0) {
        return 0;
    }
    return has_semantic_error ? 2 : 1;
}

/* ---------------------------------------------------------------------------
 * Output formatting
 * --------------------------------------------------------------------------- */

static void printResultsText(list *errors, list *warnings, aclCheckerConfig *config) {
    if (listLength(errors) == 0 && listLength(warnings) == 0) {
        return;
    }

    listIter li;
    listNode *ln;
    int i = 0;
    listRewind(errors, &li);
    while ((ln = listNext(&li)) != NULL) {
        fprintf(stderr, "Error: %s\n", (char *)listNodeValue(ln));
        if (config->verbose && i < (int)listLength(errors) - 1) {
            fprintf(stderr, "\n");
        }
        i++;
    }
    listRewind(warnings, &li);
    while ((ln = listNext(&li)) != NULL) {
        if (!config->quiet) {
            fprintf(stderr, "Warning: %s\n", (char *)listNodeValue(ln));
        }
    }

    if (!config->quiet) {
        if (listLength(errors) > 0)
            fprintf(stderr, "\n%d error(s) found.\n", (int)listLength(errors));
        if (listLength(warnings) > 0)
            fprintf(stderr, "%d warning(s) found.\n", (int)listLength(warnings));
    }
}

static void printJsonEscapedString(const char *s, size_t len) {
    putchar('"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = s[i];
        if (c == '"')
            printf("\\\"");
        else if (c == '\\')
            printf("\\\\");
        else if (c == '\n')
            printf("\\n");
        else if (c == '\r')
            printf("\\r");
        else if (c == '\t')
            printf("\\t");
        else if (c < 0x20)
            printf("\\u%04x", c);
        else
            putchar(c);
    }
    putchar('"');
}

static void printJsonStringArray(list *arr) {
    printf("[");
    listIter li;
    listNode *ln;
    int first = 1;
    listRewind(arr, &li);
    while ((ln = listNext(&li)) != NULL) {
        if (!first) {
            putchar(',');
        }
        sds s = listNodeValue(ln);
        printJsonEscapedString(s, sdslen(s));
        first = 0;
    }
    printf("]");
}

static void printResultsJson(list *errors, list *warnings, aclCheckerConfig *config) {
    printf("{");
    if (config->version_set) {
        printf("\"version\":\"%d.%d.%d\",", config->version.major, config->version.minor, config->version.patch);
    }
    printf("\"errors\":");
    printJsonStringArray(errors);
    printf(",\"warnings\":");
    printJsonStringArray(warnings);
    printf(",\"valid\":%s}\n", listLength(errors) == 0 ? "true" : "false");
}

/* ---------------------------------------------------------------------------
 * Usage and argument parsing
 * --------------------------------------------------------------------------- */

static void aclCheckerUsage(void) {
    printf(
        "Usage: valkey-check-acl [OPTIONS] <file|->\n"
        "\n"
        "Validate ACL configuration files offline.\n"
        "\n"
        "Options:\n"
        "  --level <syntax|semantic|full>  Validation level (default: semantic)\n"
        "  --version <major.minor[.patch]> Validate against a specific version's syntax\n"
        "                                  (e.g., 6.2, 7.0, 9.1). Default: current.\n"
        "  --json                          Output results as JSON\n"
        "  --verbose                       Show extra detail\n"
        "  --quiet                         Only show errors\n"
        "  --fail-fast                     Stop at first error\n"
        "  --ignore-unknown-commands       Accept commands not in the command table\n"
        "  --commands-file <path>          Load additional commands (repeatable)\n"
        "  --simplify                      Output simplified canonical ACL rules\n"
        "  -h, --help                      Show this help\n"
        "\n"
        "Input:\n"
        "  <file>   Path to an ACL file or valkey.conf\n"
        "  -        Read from stdin\n"
        "\n"
        "Exit codes:\n"
        "  0  No errors or warnings\n"
        "  1  Syntax errors found\n"
        "  2  Semantic errors (unknown commands, version violations, duplicates)\n"
        "  3  Warnings only (--level full)\n");
}

static void aclCheckerConfigInit(aclCheckerConfig *config) {
    serverAssert(config != NULL);
    memset(config, 0, sizeof(*config));
    config->level = CHECK_ACL_LEVEL_SEMANTIC;
}

static int parseArgs(int argc, char **argv, aclCheckerConfig *config) {
    aclCheckerConfigInit(config);
    int has_input = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--level") && i + 1 < argc) {
            i++;
            if (!strcasecmp(argv[i], "syntax"))
                config->level = CHECK_ACL_LEVEL_SYNTAX;
            else if (!strcasecmp(argv[i], "semantic"))
                config->level = CHECK_ACL_LEVEL_SEMANTIC;
            else if (!strcasecmp(argv[i], "full"))
                config->level = CHECK_ACL_LEVEL_FULL;
            else {
                fprintf(stderr, "Unknown level: %s\n", argv[i]);
                return -1;
            }
        } else if (!strcmp(argv[i], "--version") && i + 1 < argc) {
            i++;
            if (parseVersion(argv[i], &config->version) != 0) {
                fprintf(stderr, "Invalid version format: %s (expected major.minor or major.minor.patch)\n", argv[i]);
                return -1;
            }
            if (config->version.major < 6) {
                fprintf(stderr, "Minimum supported version is 6.0 (ACLs were introduced in Redis 6.0)\n");
                return -1;
            }
            config->version_set = 1;
        } else if (!strcmp(argv[i], "--json")) {
            config->json = 1;
        } else if (!strcmp(argv[i], "--verbose")) {
            config->verbose = 1;
        } else if (!strcmp(argv[i], "--quiet")) {
            config->quiet = 1;
        } else if (!strcmp(argv[i], "--fail-fast")) {
            config->fail_fast = 1;
        } else if (!strcmp(argv[i], "--ignore-unknown-commands")) {
            config->ignore_unknown_commands = 1;
        } else if (!strcmp(argv[i], "--simplify")) {
            config->simplify = 1;
        } else if (!strcmp(argv[i], "--commands-file") && i + 1 < argc) {
            i++;
            if (config->num_commands_files >= 16) {
                fprintf(stderr, "Too many --commands-file arguments (max 16)\n");
                return -1;
            }
            config->commands_files[config->num_commands_files++] = argv[i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            aclCheckerUsage();
            return 1; /* Signal to exit with 0. */
        } else if (argv[i][0] == '-' && strcmp(argv[i], "-")) {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        } else {
            if (config->filename != NULL) {
                fprintf(stderr, "Multiple input files not supported.\n");
                return -1;
            }
            /* "-" means stdin (filename stays NULL), otherwise store the path */
            if (strcmp(argv[i], "-") != 0) {
                config->filename = argv[i];
            }
            has_input = 1;
        }
    }

    if (!has_input) {
        fprintf(stderr, "No input file specified. Use '-' for stdin.\n\n");
        aclCheckerUsage();
        return -1;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Entry point
 * --------------------------------------------------------------------------- */

int valkey_check_acl_main(int argc, char **argv) {
    aclCheckerConfig config;
    int ret = parseArgs(argc, argv, &config);
    if (ret != 0) {
        return ret < 0 ? 1 : 0;
    }

    /* Load external commands files. */
    dict *ext_commands;
    ext_commands = dictCreate(&extCommandsDictType);
    for (int i = 0; i < config.num_commands_files; i++) {
        if (loadCommandsFile(config.commands_files[i], ext_commands) != 0) {
            dictRelease(ext_commands);
            return 1;
        }
    }

    /* Initialize pubsub structures needed by ACLStringSetUser's
     * ACLKillPubsubClientsIfNeeded path. */
    server.pubsub_channels = kvstoreCreate(&kvstoreChannelHashtableType, 0, KVSTORE_ALLOCATE_HASHTABLES_ON_DEMAND);
    server.pubsub_patterns = dictCreate(&objToHashtableDictType);
    server.pubsubshard_channels =
        kvstoreCreate(&kvstoreChannelHashtableType, 0, KVSTORE_ALLOCATE_HASHTABLES_ON_DEMAND);
    server.clients = listCreate();

    /* Read input. */
    sds content = readFileContent(config.filename);
    if (!content) {
        fprintf(stderr, "Error opening '%s': %s\n", config.filename, strerror(errno));
        dictRelease(ext_commands);
        return 1;
    }

    if (sdslen(content) == 0) {
        sdsfree(content);
        dictRelease(ext_commands);
        if (config.json)
            printf("{\"errors\":[],\"warnings\":[],\"valid\":true}\n");
        else if (!config.quiet)
            printf("ACL file is valid.\n");
        return 0;
    }

    const char *display_name = config.filename == NULL ? "<stdin>" : config.filename;

    list *errors;
    list *warnings;
    errors = listCreate();
    warnings = listCreate();

    sds simplified = config.simplify ? sdsempty() : NULL;

    int validation_result = validateContent(content, display_name, &config, ext_commands, errors, warnings, &simplified);
    sdsfree(content);
    dictRelease(ext_commands);

    /* If --simplify and validation passed, output simplified rules. */
    if (config.simplify && listLength(errors) == 0 && simplified) {
        if (config.json) {
            printf("{\"simplified\":");
            printJsonEscapedString(simplified, sdslen(simplified));
            printf(",\"warnings\":");
            printJsonStringArray(warnings);
            printf(",\"valid\":true}\n");
        } else {
            printf("%s", simplified);
            /* Print warnings to stderr in text mode. */
            if (listLength(warnings) > 0)
                printResultsText(errors, warnings, &config);
        }
    }
    if (simplified) {
        sdsfree(simplified);
    }

    /* Output validation results (errors/warnings) when not in simplify mode. */
    if (!config.simplify || listLength(errors) > 0) {
        if (listLength(errors) > 0 || listLength(warnings) > 0) {
            if (config.json)
                printResultsJson(errors, warnings, &config);
            else
                printResultsText(errors, warnings, &config);
        } else if (!config.simplify) {
            if (config.json)
                printResultsJson(errors, warnings, &config);
            else if (!config.quiet)
                printf("ACL file is valid.\n");
        }
    }

    /* Determine exit code: 0=OK, 1=syntax errors, 2=semantic errors, 3=warnings only. */
    int exit_code = 0;
    if (validation_result == 2)
        exit_code = 2;
    else if (listLength(errors) > 0)
        exit_code = 1;
    else if (listLength(warnings) > 0)
        exit_code = 3;

    aclCheckerListFree(errors);
    aclCheckerListFree(warnings);
    return exit_code;
}
