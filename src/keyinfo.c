#include "keyinfo.h"

void removeNodeFromList(dataType previous_type, robj *keyobj) {
    list *process_list = NULL;
    if (previous_type == STRING_TYPE) {
        process_list = server.string_bigkey_info;
    } else if (previous_type == LIST_TYPE) {
        process_list = server.list_bigkey_info;
    } else if (previous_type == HASH_TYPE) {
        process_list = server.hash_bigkey_info;
    } else if (previous_type == SET_TYPE) {
        process_list = server.set_bigkey_info;
    } else if (previous_type == SORTED_SET_TYPE) {
        process_list = server.zset_bigkey_info;
    } else {
        // TO DO later
    }

    listIter li;
    listNode *ln;
    listRewind(process_list, &li);
    while ((ln = listNext(&li)) != NULL) {
        bigkeyEntry *node = ln->value;
        if (sdscmp(node->key->ptr, keyobj->ptr) != 0) continue;
        decrRefCount(node->key);
        listDelNode(process_list, ln);
        zfree(node);
    }
}

void addNodeToList(dataType new_type, robj *keyobj, long curr) {
    list *process_list = NULL;
    if (new_type == STRING_TYPE) {
        process_list = server.string_bigkey_info;
    } else if (new_type == LIST_TYPE) {
        process_list = server.list_bigkey_info;
    } else if (new_type == HASH_TYPE) {
        process_list = server.hash_bigkey_info;
    } else if (new_type == SET_TYPE) {
        process_list = server.set_bigkey_info;
    } else if (new_type == SORTED_SET_TYPE) {
        process_list = server.zset_bigkey_info;
    } else {
        // TO DO later
    }

    bigkeyEntry *bke = zmalloc(sizeof(bigkeyEntry));
    incrRefCount(keyobj);

    bke->value = curr;
    bke->key = keyobj;

    if (listLength(process_list) == 0) {
        listAddNodeHead(process_list, bke);
    } else {
        listIter li;
        listNode *ln;
        listRewind(process_list, &li);
        int isInsert = 0;
        while ((ln = listNext(&li)) != NULL) {
            bigkeyEntry *node = ln->value;
            if (curr > node->value) continue;
            listInsertNode(process_list, ln, bke, 0);
            isInsert = 1;
            break;
        }
        if (!isInsert) listAddNodeTail(process_list, bke);
    }
}


void updateBigKeyList(robj *keyobj, long previous, long curr, dataType type) {
    if (type == STRING_TYPE) {
        if (previous < server.string_memory_use && curr < server.string_memory_use) {
            return;
        } else if (previous >= server.string_memory_use && curr < server.string_memory_use) {
            removeNodeFromList(type, keyobj);
        } else if (previous < server.string_memory_use && curr >= server.string_memory_use) {
            addNodeToList(type, keyobj, curr);
        } else if (previous >= server.string_memory_use && curr >= server.string_memory_use) {
            removeNodeFromList(type, keyobj);
            addNodeToList(type, keyobj, curr);
        }
    } else {
        if (previous < server.big_key_number_element && curr < server.big_key_number_element) {
            return;
        } else if (previous >= server.big_key_number_element && curr < server.big_key_number_element) {
            removeNodeFromList(type, keyobj);
        } else if (previous < server.big_key_number_element && curr >= server.big_key_number_element) {
            addNodeToList(type, keyobj, curr);
        } else if (previous >= server.big_key_number_element && curr >= server.big_key_number_element) {
            removeNodeFromList(type, keyobj);
            addNodeToList(type, keyobj, curr);
        }
    }
}

void bigkeyListInit(void) {
    server.string_bigkey_info = listCreate();
    server.list_bigkey_info = listCreate();
    server.hash_bigkey_info = listCreate();
    server.set_bigkey_info = listCreate();
    server.zset_bigkey_info = listCreate();
}

// bigKeyInfo [string | set | hash | list | zset | xstream | all]
void bigkeyInfoCommand(client *c) {
    bool isDisplayAll = 0;
    bool isDisplayString = 0;
    bool isDisplaySet = 0;
    bool isDisplayHash = 0;
    bool isDisplayList = 0;
    bool isDisplayZset = 0;
    long totalLine = 0;

    long count_string = listLength(server.string_bigkey_info);
    long count_list = listLength(server.list_bigkey_info);
    long count_hash = listLength(server.hash_bigkey_info);
    long count_set = listLength(server.set_bigkey_info);
    long count_zset = listLength(server.zset_bigkey_info);

    if (c->argc == 1) {
        isDisplayAll = 1;
    } else {
        int j = 1;
        while (j < c->argc) {
            char *opt = c->argv[j]->ptr;
            if (!strcasecmp(opt, "string")) {
                isDisplayString = 1;
            } else if (!strcasecmp(opt, "set")) {
                isDisplaySet = 1;
            } else if (!strcasecmp(opt, "hash")) {
                isDisplayHash = 1;
            } else if (!strcasecmp(opt, "list")) {
                isDisplayList = 1;
            } else if (!strcasecmp(opt, "zset")) {
                isDisplayZset = 1;
            } else if (!strcasecmp(opt, "all")) {
                isDisplayAll = 1;
                break;
            } else {
                addReplyErrorFormat(c, "Unsupported option %s", opt);
                return;
            }
            j++;
        }
    }

    if (server.big_key_output < count_string) {
        count_string = server.big_key_output;
    }
    if (server.big_key_output < count_list) {
        count_list = server.big_key_output;
    }
    if (server.big_key_output < count_hash) {
        count_hash = server.big_key_output;
    }
    if (server.big_key_output < count_set) {
        count_set = server.big_key_output;
    }
    if (server.big_key_output < count_zset) {
        count_zset = server.big_key_output;
    }

    if (isDisplayAll | isDisplayString) {
        totalLine += count_string;
    }
    if (isDisplayAll | isDisplaySet) {
        totalLine += count_set;
    }
    if (isDisplayAll | isDisplayHash) {
        totalLine += count_hash;
    }
    if (isDisplayAll | isDisplayList) {
        totalLine += count_list;
    }
    if (isDisplayAll | isDisplayZset) {
        totalLine += count_zset;
    }

    addReplyArrayLen(c, totalLine);

    listIter li;
    listNode *ln;

    if (isDisplayAll | isDisplayString) {
        listRewindTail(server.string_bigkey_info, &li);
        while (count_string--) {
            ln = listNext(&li);
            bigkeyEntry *bke = ln->value;
            addReplyArrayLen(c, 3);
            addReplyLongLong(c, bke->value);
            addReplyBulkCBuffer(c, bke->key->ptr, sdslen(bke->key->ptr));
            addReplyBulkCString(c, "string");
        }
    }

    if (isDisplayAll | isDisplayList) {
        listRewindTail(server.list_bigkey_info, &li);
        while (count_list--) {
            ln = listNext(&li);
            bigkeyEntry *bke = ln->value;
            addReplyArrayLen(c, 3);
            addReplyLongLong(c, bke->value);
            addReplyBulkCBuffer(c, bke->key->ptr, sdslen(bke->key->ptr));
            addReplyBulkCString(c, "list");
        }
    }

    if (isDisplayAll | isDisplayHash) {
        listRewindTail(server.hash_bigkey_info, &li);
        while (count_hash--) {
            ln = listNext(&li);
            bigkeyEntry *bke = ln->value;
            addReplyArrayLen(c, 3);
            addReplyLongLong(c, bke->value);
            addReplyBulkCBuffer(c, bke->key->ptr, sdslen(bke->key->ptr));
            addReplyBulkCString(c, "hash");
        }
    }

    if (isDisplayAll | isDisplaySet) {
        listRewindTail(server.set_bigkey_info, &li);
        while (count_set--) {
            ln = listNext(&li);
            bigkeyEntry *bke = ln->value;
            addReplyArrayLen(c, 3);
            addReplyLongLong(c, bke->value);
            addReplyBulkCBuffer(c, bke->key->ptr, sdslen(bke->key->ptr));
            addReplyBulkCString(c, "set");
        }
    }

    if (isDisplayAll | isDisplayZset) {
        listRewindTail(server.list_bigkey_info, &li);
        while (count_zset--) {
            ln = listNext(&li);
            bigkeyEntry *bke = ln->value;
            addReplyArrayLen(c, 3);
            addReplyLongLong(c, bke->value);
            addReplyBulkCBuffer(c, bke->key->ptr, sdslen(bke->key->ptr));
            addReplyBulkCString(c, "Sorted Set");
        }
    }
}
