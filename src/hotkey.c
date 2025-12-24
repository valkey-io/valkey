#include "server.h"

/* HOTKEYS GET [TYPE {read|write|all}] */
void hotkeysGetCommand(client *c) {
    /* Check if hotkey detection is enabled */
    if (!server.hotkey_enabled) {
        addReplyError(c, "Hotkey detection is disabled");
        return;
    }

    if (!server.hotkey_manager || !server.hotkey_manager->history_lru) {
        addReplyArrayLen(c, 0);
        return;
    }

    /* Parse parameters */
    int filter_type = -1; /* -1: all, 0: write, 1: read */

    if (c->argc > 2) {
        if (c->argc == 4 && !strcasecmp(c->argv[2]->ptr, "TYPE")) {
            char *type_str = c->argv[3]->ptr;
            if (!strcasecmp(type_str, "read")) {
                filter_type = 1;
            } else if (!strcasecmp(type_str, "write")) {
                filter_type = 0;
            } else if (!strcasecmp(type_str, "all")) {
                filter_type = -1;
            } else {
                addReplyError(c, "Invalid type. Use 'read', 'write', or 'all'");
                return;
            }
        } else {
            addReplyError(c, "Syntax error. Usage: HOTKEYS GET [TYPE {read|write|all}]");
            return;
        }
    }

    /* First clean up expired historical records */
    expireHotkeyHistory(server.hotkey_manager);

    /* Check again if manager is valid */
    if (!server.hotkey_manager || !server.hotkey_manager->history_lru) {
        addReplyArrayLen(c, 0);
        return;
    }

    /* Count the number of hotkeys that meet the criteria */
    int count = 0;
    hotkeyLRUNode *current = server.hotkey_manager->history_lru->head;
    while (current) {
        if (current->entry && (filter_type == -1 || current->entry->is_read == filter_type)) {
            count++;
        }
        current = current->next;
    }

    /* Send array length */
    addReplyArrayLen(c, count);

    /* Send hotkey information in LRU order (from newest to oldest) */
    current = server.hotkey_manager->history_lru->head;
    while (current) {
        /* Safety check: ensure both node and entry are valid */
        if (!current->entry || !current->key || (filter_type != -1 && current->entry->is_read != filter_type)) {
            current = current->next;
            continue;
        }

        /* Each hotkey returns an array containing detailed information */
        addReplyArrayLen(c, 12);

        /* key */
        addReplyBulkCString(c, "key");
        addReplyBulkCString(c, current->key);

        /* type */
        addReplyBulkCString(c, "type");
        addReplyBulkCString(c, current->entry->is_read ? "read" : "write");

        /* peak_qps */
        addReplyBulkCString(c, "peak_qps");
        addReplyLongLong(c, current->entry->peak_qps);

        /* first_detected */
        addReplyBulkCString(c, "first_detected");
        addReplyLongLong(c, current->entry->first_detected);

        /* last_detected */
        addReplyBulkCString(c, "last_detected");
        addReplyLongLong(c, current->entry->last_detected);

        /* duration */
        addReplyBulkCString(c, "duration");
        addReplyLongLong(c, current->entry->duration);

        current = current->next;
    }
}

/* HOTKEYS RESET */
void hotkeysResetCommand(client *c) {
    /* Check if hotkey detection is enabled */
    if (!server.hotkey_enabled) {
        addReplyError(c, "Hotkey detection is disabled");
        return;
    }

    if (server.hotkey_manager) {
        /* Clear historical record dictionary - the dictionary's val destructor will automatically release LRU node memory */
        if (server.hotkey_manager->history_dict) {
            dictEmpty(server.hotkey_manager->history_dict, NULL);
        }

        /* Reinitialize LRU list (without calling hotkeyLRUFree to avoid double release) */
        if (server.hotkey_manager->history_lru) {
            /* Directly reset LRU structure since nodes have been released by dictionary destructor */
            server.hotkey_manager->history_lru->head = NULL;
            server.hotkey_manager->history_lru->tail = NULL;
            server.hotkey_manager->history_lru->size = 0;
        }

        /* Reset CMS and current hotkeys (excluding historical records) */
        hotkeyManagerReset(server.hotkey_manager);

        /* Update statistics - only reset historical record related statistics, keep cumulative statistics */
        server.hotkey_runtime_history_count = 0;
    }

    addReply(c, shared.ok);
}

/* HOTKEYS command dispatcher */
void hotkeysCommand(client *c) {
    if (c->argc < 2) {
        addReplyError(c, "Wrong number of arguments for 'HOTKEYS' command");
        return;
    }

    char *subcmd = c->argv[1]->ptr;

    if (!strcasecmp(subcmd, "get")) {
        hotkeysGetCommand(c);
    } else if (!strcasecmp(subcmd, "reset")) {
        hotkeysResetCommand(c);
    } else {
        addReplyErrorFormat(c, "Unknown HOTKEYS subcommand '%s'", subcmd);
    }
}

uint32_t murmurHash2(const void *key, int len, uint32_t seed) {
    /* 'm' and 'r' are mixing constants generated offline.
     * They're not really 'magic', they just happen to work well.
     */
    const uint32_t m = 0x5bd1e995;
    const int r = 24;

    /* Initialize the hash to a 'random' value */
    uint32_t h = seed ^ len;

    /* Mix 4 bytes at a time into the hash */
    const unsigned char *data = (const unsigned char *)key;
    while (len >= 4) {
        uint32_t k = *(uint32_t *)data;

        k *= m;
        k ^= k >> r;
        k *= m;

        h *= m;
        h ^= k;

        data += 4;
        len -= 4;
    }

    /* Handle the last few bytes of the input array */
    switch (len) {
    case 3:
        h ^= data[2] << 16;
        /* fallthrough */
    case 2:
        h ^= data[1] << 8;
        /* fallthrough */
    case 1:
        h ^= data[0];
        h *= m;
    };

    /* Do a few final mixes of the hash to ensure the last few
     * bytes are well-incorporated.
     */
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;

    return h;
}

/* Calculate the nearest power of two */
static size_t nextPowerOfTwo(size_t n) {
    if (n == 0) return 1;
    /* Already a power of two */
    if ((n & (n - 1)) == 0) return n;

    size_t power = 1;
    while (power < n) {
        power <<= 1;
    }
    return power;
}

hotkeyCMS *newHotkeyCMS(size_t width, size_t depth) {
    serverAssert(width > 0);
    serverAssert(depth > 0);

    hotkeyCMS *hotkey_cms = (hotkeyCMS *)zcalloc(sizeof(hotkeyCMS));

    /* Automatically adjust width to the nearest power of two */
    size_t adjusted_width = nextPowerOfTwo(width);

    hotkey_cms->width = adjusted_width;
    hotkey_cms->depth = depth;
    hotkey_cms->counter = 0;
    /* Used for bitwise operation optimization of modulo */
    hotkey_cms->width_mask = adjusted_width - 1;
    hotkey_cms->array = (uint32_t *)zcalloc(adjusted_width * depth * sizeof(uint32_t));

    return hotkey_cms;
}

void freeHotkeyCMS(hotkeyCMS *hotkey_cms) {
    if (!hotkey_cms) {
        return;
    }

    if (hotkey_cms->array) {
        zfree(hotkey_cms->array);
        hotkey_cms->array = NULL;
    }

    zfree(hotkey_cms);
}

/* Create LRU list manager */
static hotkeyLRU *hotkeyLRUInit(void) {
    hotkeyLRU *lru = zmalloc(sizeof(hotkeyLRU));
    if (!lru) {
        return NULL;
    }

    lru->head = NULL;
    lru->tail = NULL;
    lru->size = 0;

    return lru;
}

/* Move node to the head of the list (newest position) */
static void hotkeyLRUMoveToHead(hotkeyLRU *lru, hotkeyLRUNode *node) {
    if (!lru || !node || lru->head == node) {
        return;
    }

    /* Remove from current position */
    if (node->prev) {
        node->prev->next = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }
    if (lru->tail == node) {
        lru->tail = node->prev;
    }

    /* Move to head */
    node->prev = NULL;
    node->next = lru->head;
    if (lru->head) {
        lru->head->prev = node;
    }
    lru->head = node;

    /* If list is empty, set tail */
    if (!lru->tail) {
        lru->tail = node;
    }
}

/* Add new node to the head of the list */
static hotkeyLRUNode *hotkeyLRUAddToHead(hotkeyLRU *lru, sds key, hotkeyHistoryEntry *entry) {
    if (!lru) {
        return NULL;
    }

    hotkeyLRUNode *node = zmalloc(sizeof(hotkeyLRUNode));
    if (!node) {
        return NULL;
    }

    node->key = key;
    node->entry = entry;
    node->prev = NULL;
    node->next = lru->head;

    if (lru->head) {
        lru->head->prev = node;
    }
    lru->head = node;

    if (!lru->tail) {
        lru->tail = node;
    }

    lru->size++;
    return node;
}

/* Remove the tail node (oldest) from the list */
static hotkeyLRUNode *hotkeyLRURemoveTail(hotkeyLRU *lru) {
    if (!lru || !lru->tail) {
        return NULL;
    }

    hotkeyLRUNode *tail = lru->tail;

    if (tail->prev) {
        tail->prev->next = NULL;
        lru->tail = tail->prev;
    } else {
        lru->head = NULL;
        lru->tail = NULL;
    }

    lru->size--;
    return tail;
}

hotkeyManager *hotkeyManagerInit(size_t cms_width, size_t cms_depth) {
    hotkeyManager *manager = zmalloc(sizeof(hotkeyManager));
    if (!manager) {
        return NULL;
    }

    /* Initialize all pointers to NULL for easier cleanup during error handling */
    manager->read_hotkeys_cms = NULL;
    manager->read_hotkeys = NULL;
    manager->write_hotkeys_cms = NULL;
    manager->write_hotkeys = NULL;
    manager->history_dict = NULL;
    manager->history_lru = NULL;
    manager->read_hotkey_cms_threshold = 0;
    manager->write_hotkey_cms_threshold = 0;

    /* Create read operation CMS */
    manager->read_hotkeys_cms = newHotkeyCMS(cms_width, cms_depth);
    if (!manager->read_hotkeys_cms) {
        hotkeyManagerFree(manager);
        return NULL;
    }

    /* Create read hotkey dictionary */
    manager->read_hotkeys = dictCreate(&hotKeyDictType);
    if (!manager->read_hotkeys) {
        hotkeyManagerFree(manager);
        return NULL;
    }

    /* Create write operation CMS */
    manager->write_hotkeys_cms = newHotkeyCMS(cms_width, cms_depth);
    if (!manager->write_hotkeys_cms) {
        hotkeyManagerFree(manager);
        return NULL;
    }

    /* Create write hotkey dictionary */
    manager->write_hotkeys = dictCreate(&hotKeyDictType);
    if (!manager->write_hotkeys) {
        hotkeyManagerFree(manager);
        return NULL;
    }

    /* Create history dictionary (stores key -> hotkeyLRUNode mapping) */
    manager->history_dict = dictCreate(&hotkeyHistoryDictType);
    if (!manager->history_dict) {
        hotkeyManagerFree(manager);
        return NULL;
    }

    /* Create LRU list manager */
    manager->history_lru = hotkeyLRUInit();
    if (!manager->history_lru) {
        hotkeyManagerFree(manager);
        return NULL;
    }

    return manager;
}

void hotkeyManagerFree(hotkeyManager *manager) {
    if (!manager) {
        return;
    }

    /* Release read operation CMS */
    if (manager->read_hotkeys_cms) {
        freeHotkeyCMS(manager->read_hotkeys_cms);
        manager->read_hotkeys_cms = NULL;
    }

    /* Release read hotkey dictionary */
    if (manager->read_hotkeys) {
        dictRelease(manager->read_hotkeys);
        manager->read_hotkeys = NULL;
    }

    /* Release write operation CMS */
    if (manager->write_hotkeys_cms) {
        freeHotkeyCMS(manager->write_hotkeys_cms);
        manager->write_hotkeys_cms = NULL;
    }

    /* Release write hotkey dictionary */
    if (manager->write_hotkeys) {
        dictRelease(manager->write_hotkeys);
        manager->write_hotkeys = NULL;
    }

    /* Release history dictionary (will automatically release all LRU nodes through dictHotkeyLRUNodeDestructor) */
    if (manager->history_dict) {
        dictRelease(manager->history_dict);
        manager->history_dict = NULL;
    }

    /* Only release the LRU list manager itself, not the nodes (nodes have been released by dictionary) */
    if (manager->history_lru) {
        zfree(manager->history_lru);
        manager->history_lru = NULL;
    }

    /* Free the manager itself */
    zfree(manager);
}

size_t hotkeyCMSUpdate(hotkeyCMS *hotkey_cms, robj *key) {
    if (!hotkey_cms || !key || !key->ptr) {
        return 0;
    }

    size_t minCount = (size_t)-1;

    size_t len = stringObjectLen(key);

    /* Avoid hashing empty strings */
    if (len == 0) {
        return 0;
    }

    for (size_t i = 0; i < hotkey_cms->depth; ++i) {
        uint32_t hash = murmurHash2(key->ptr, len, i);
        /* Use bitwise operation to optimize modulo: hash & (width - 1) equals hash % width (when width is a power of 2) */
        size_t loc = (hash & hotkey_cms->width_mask) + (i * hotkey_cms->width);
        hotkey_cms->array[loc]++;
        minCount = min(minCount, hotkey_cms->array[loc]);
    }
    hotkey_cms->counter++;
    return minCount;
}

static hotkeyStatEntry *getOrCreateHotkeyStatEntry(dict *hotkeys, robj *key) {
    if (!hotkeys || !key || !key->ptr) {
        return NULL;
    }

    dictEntry *de = dictFind(hotkeys, key->ptr);
    if (de) {
        hotkeyStatEntry *entry = dictGetVal(de);
        return entry;
    }

    /* If the key does not exist in the dictionary, create a new statistics entry */
    hotkeyStatEntry *entry = zmalloc(sizeof(hotkeyStatEntry));
    if (!entry) {
        return NULL;
    }
    entry->current_count = 0;

    sds key_sds = sdsnew(key->ptr);
    if (!key_sds) {
        zfree(entry);
        return NULL;
    }

    if (dictAdd(hotkeys, key_sds, entry) != DICT_OK) {
        sdsfree(key_sds);
        zfree(entry);
        return NULL;
    }

    return entry;
}

/* Calculate actual QPS: Estimate actual QPS based on sample count, sampling ratio, and time window. For example:
 * - Time window = 1 second
 * - Sampling ratio = 10%
 * - Sample count = 50
 * - Actual QPS = (50 * 100 / 10) / 1 = 500
 */
static uint64_t calculateActualQPS(hotkeyStatEntry *stat_entry) {
    if (!stat_entry || server.hotkey_sampling_ratio <= 0 || server.hotkey_window_seconds <= 0) {
        return 0;
    }

    /* Calculate actual QPS: (sample count * 100 / sampling ratio) / time window */
    uint64_t total_estimated_requests = (stat_entry->current_count * 100) / server.hotkey_sampling_ratio;
    uint64_t actual_qps = total_estimated_requests / server.hotkey_window_seconds;

    return actual_qps;
}

void readHotKeyDetection(robj *key, int val_type) {
    if (!server.hotkey_manager || !key) {
        return;
    }

    server.hotkey_runtime_total_sampled++;

    size_t count = hotkeyCMSUpdate(server.hotkey_manager->read_hotkeys_cms, key);

    /* If the counter does not exceed the threshold, consider it not a hot key */
    if (count < server.hotkey_manager->read_hotkey_cms_threshold) {
        return;
    }

    /* If the counter exceeds the threshold, perform hot key collection */
    hotkeyStatEntry *entry = getOrCreateHotkeyStatEntry(server.hotkey_manager->read_hotkeys, key);
    if (entry) {
        entry->current_count = count;
        entry->val_type = val_type;
    }
}

void writeHotKeyDetection(robj *key, int val_type) {
    if (!server.hotkey_manager || !key) {
        return;
    }

    server.hotkey_runtime_total_sampled++;

    size_t count = hotkeyCMSUpdate(server.hotkey_manager->write_hotkeys_cms, key);

    /* If the counter does not exceed the threshold, consider it not a hot key */
    if (count < server.hotkey_manager->write_hotkey_cms_threshold) {
        return;
    }

    /* If the counter exceeds the threshold, perform hot key collection */
    hotkeyStatEntry *entry = getOrCreateHotkeyStatEntry(server.hotkey_manager->write_hotkeys, key);
    if (entry) {
        entry->current_count = count;
        entry->val_type = val_type;
    }
}

void hotkeyCMSReset(hotkeyCMS *hotkey_cms) {
    if (!hotkey_cms || !hotkey_cms->array) {
        return;
    }

    size_t total_size = (size_t)hotkey_cms->width * hotkey_cms->depth * sizeof(uint32_t);
    memset(hotkey_cms->array, 0, total_size);
    hotkey_cms->counter = 0;
}

/* Evict the least recently used historical record entry (LRU policy) */
static void evictLRUHistoryEntry(hotkeyManager *manager) {
    if (!manager || !manager->history_lru || !manager->history_dict || manager->history_lru->size == 0) {
        return;
    }

    /* Remove the tail node from the LRU list (oldest) */
    hotkeyLRUNode *tail = hotkeyLRURemoveTail(manager->history_lru);
    if (!tail) {
        return;
    }

    /* Delete the corresponding entry from the dictionary - the dictionary's val destructor will automatically release node memory */
    if (tail->key && manager->history_dict) {
        dictDelete(manager->history_dict, tail->key);
    } else {
        /* If dictionary deletion fails, manually release memory */
        if (tail->key) {
            sdsfree(tail->key);
        }
        if (tail->entry) {
            zfree(tail->entry);
        }
        zfree(tail);
    }
}

/* Add hot key to historical records (using LRU management) */
static void addSingleHotkeyToHistory(hotkeyManager *manager, const char *key_str, hotkeyStatEntry *stat_entry, int is_read) {
    if (!manager || !key_str || !stat_entry || !manager->history_dict || !manager->history_lru) {
        return;
    }

    time_t now = time(NULL);

    /* Check if historical record for this key already exists */
    dictEntry *de = dictFind(manager->history_dict, key_str);
    if (de) {
        /* Update existing record */
        hotkeyLRUNode *node = dictGetVal(de);
        if (!node || !node->entry) {
            return;
        }

        hotkeyHistoryEntry *history_entry = node->entry;
        uint64_t actual_qps = calculateActualQPS(stat_entry);
        if (history_entry->peak_qps < actual_qps) {
            history_entry->peak_qps = actual_qps;
        }
        history_entry->last_detected = now;
        history_entry->duration += server.hotkey_window_seconds;
        history_entry->val_type = stat_entry->val_type;

        /* Move the node to the head of the LRU list (mark as most recently used) */
        hotkeyLRUMoveToHead(manager->history_lru, node);
        return;
    }

    /* Check if the maximum historical record count is exceeded, if so, evict using LRU */
    while (manager->history_lru->size >= (size_t)server.hotkey_history_max_count) {
        size_t old_size = manager->history_lru->size;
        evictLRUHistoryEntry(manager);
        /* Prevent infinite loop, exit if eviction fails */
        if (manager->history_lru->size >= old_size) {
            break;
        }
    }

    /* Create a new historical record entry */
    hotkeyHistoryEntry *history_entry = zmalloc(sizeof(hotkeyHistoryEntry));
    if (!history_entry) {
        return;
    }

    history_entry->peak_qps = calculateActualQPS(stat_entry);
    history_entry->first_detected = now;
    history_entry->last_detected = now;
    history_entry->is_read = is_read;
    history_entry->duration = server.hotkey_window_seconds;
    history_entry->val_type = stat_entry->val_type;

    /* Copy key string */
    sds key_sds = sdsnew(key_str);
    if (!key_sds) {
        zfree(history_entry);
        return;
    }

    /* Add to the head of LRU list */
    hotkeyLRUNode *node = hotkeyLRUAddToHead(manager->history_lru, key_sds, history_entry);
    if (!node) {
        sdsfree(key_sds);
        zfree(history_entry);
        return;
    }

    /* Add to historical record dictionary (key -> LRU node mapping) */
    if (dictAdd(manager->history_dict, key_sds, node) != DICT_OK) {
        /* If dictionary addition fails, need to clean up LRU node, remove the newly added node from LRU list */
        if (manager->history_lru->head == node) {
            manager->history_lru->head = node->next;
            if (node->next) {
                node->next->prev = NULL;
            } else {
                manager->history_lru->tail = NULL;
            }
            manager->history_lru->size--;
        }
        sdsfree(key_sds);
        zfree(history_entry);
        zfree(node);
        return;
    }
}

/* Publish single hot key notification */
static void publishSingleHotkeyNotification(const char *key_str, hotkeyStatEntry *stat_entry, int is_read) {
    if (!key_str || !stat_entry) {
        return;
    }

    /* Use the new QPS calculation function */
    uint64_t actual_qps = calculateActualQPS(stat_entry);

    /* Build hot key notification message in JSON format */
    sds message = sdsempty();
    message = sdscatprintf(message,
                           "{\"key\":\"%s\",\"type\":\"%s\",\"qps\":%llu,\"val_type\":%d,\"timestamp\":%ld}",
                           key_str,
                           is_read ? "read" : "write",
                           (unsigned long long)actual_qps,
                           stat_entry->val_type,
                           (long)time(NULL));

    if (!message) {
        return;
    }

    /* Use shared channel object to avoid duplicate creation */
    robj *msg = createObject(OBJ_STRING, message);

    if (shared.hotkey_notify_channel && msg) {
        pubsubPublishMessage(shared.hotkey_notify_channel, msg, 0);
    }

    /* Always need to release message object, because pubsubPublishMessage does not manage message object reference counting */
    if (msg) decrRefCount(msg);
}

/* Execute once per time window, add hot keys within this time window to the hot key historical set and publish notifications */
void addHotkeyToHistory(hotkeyManager *manager) {
    if (!manager) {
        return;
    }

    /* Process read hot keys */
    if (manager->read_hotkeys) {
        dictIterator *di = dictGetIterator(manager->read_hotkeys);
        dictEntry *de;

        while ((de = dictNext(di)) != NULL) {
            const char *key_str = dictGetKey(de);
            hotkeyStatEntry *stat_entry = dictGetVal(de);

            /* Add to historical records */
            addSingleHotkeyToHistory(manager, key_str, stat_entry, 1); /* is_read = 1 */

            /* Publish hot key notification */
            publishSingleHotkeyNotification(key_str, stat_entry, 1);

            /* Update runtime statistics */
            server.hotkey_runtime_read_count++;
        }

        dictReleaseIterator(di);
    }

    /* Process write hot keys */
    if (manager->write_hotkeys) {
        dictIterator *di = dictGetIterator(manager->write_hotkeys);
        dictEntry *de;

        while ((de = dictNext(di)) != NULL) {
            const char *key_str = dictGetKey(de);
            hotkeyStatEntry *stat_entry = dictGetVal(de);

            /* Add to historical records */
            addSingleHotkeyToHistory(manager, key_str, stat_entry, 0); /* is_read = 0 */

            /* Publish hotkey notification */
            publishSingleHotkeyNotification(key_str, stat_entry, 0);

            /* Update runtime statistics */
            server.hotkey_runtime_write_count++;
        }

        dictReleaseIterator(di);
    }

    /* Update historical record count statistics */
    server.hotkey_runtime_history_count = manager->history_lru->size;
}

/* Clean up expired historical records (start checking from the tail of the LRU list) */
void expireHotkeyHistory(hotkeyManager *manager) {
    if (!manager || !manager->history_lru || !manager->history_dict) {
        return;
    }

    time_t now = time(NULL);
    time_t expire_threshold = now - server.hotkey_history_ttl;

    /* Start checking expired entries from the tail of LRU list */
    hotkeyLRUNode *current = manager->history_lru->tail;
    while (current) {
        hotkeyLRUNode *prev = current->prev;

        /* Check if expired */
        if (current->entry && current->entry->last_detected < expire_threshold) {
            /* Remove from LRU list */
            if (current->prev) {
                current->prev->next = current->next;
            } else {
                manager->history_lru->head = current->next;
            }

            if (current->next) {
                current->next->prev = current->prev;
            } else {
                manager->history_lru->tail = current->prev;
            }

            manager->history_lru->size--;

            /* Delete from dictionary - this will automatically release all memory through dictHotkeyLRUNodeDestructor */
            if (current->key && manager->history_dict) {
                dictDelete(manager->history_dict, current->key);
            }

            /* Note: No need to manually release memory, dictDelete will automatically release through destructor */
        } else {
            /* Since the LRU list is time-ordered, if the current node has not expired,
            then the previous nodes will not have expired either, can exit early */
            break;
        }

        current = prev;
    }

    /* Update historical record count statistics */
    server.hotkey_runtime_history_count = manager->history_lru->size;
}

void hotkeyManagerReset(hotkeyManager *manager) {
    if (!manager) {
        return;
    }

    /* Reset read operation CMS */
    if (manager->read_hotkeys_cms) {
        hotkeyCMSReset(manager->read_hotkeys_cms);
    }

    /* Clear read hot key dictionary */
    if (manager->read_hotkeys) {
        dictEmpty(manager->read_hotkeys, NULL);
    }

    /* Reset write operation CMS */
    if (manager->write_hotkeys_cms) {
        hotkeyCMSReset(manager->write_hotkeys_cms);
    }

    /* Clear write hot key dictionary */
    if (manager->write_hotkeys) {
        dictEmpty(manager->write_hotkeys, NULL);
    }
}

static void updateHotkeyCMSThreshold(void) {
    if (!server.hotkey_manager) {
        return;
    }
    server.hotkey_manager->read_hotkey_cms_threshold = server.hotkey_read_threshold * server.hotkey_window_seconds * server.hotkey_sampling_ratio / 100;
    server.hotkey_manager->write_hotkey_cms_threshold = server.hotkey_write_threshold * server.hotkey_window_seconds * server.hotkey_sampling_ratio / 100;
}

/* Hotkey config callbacks */
int hotKeyEnabledCallback(const char **err) {
    UNUSED(err);

    if (server.hotkey_enabled) {
        if (!server.hotkey_manager) {
            /* Ensure bucket size is a power of 2 */
            size_t original_size = server.hotkey_cms_bucket_size;
            size_t adjusted_size = nextPowerOfTwo(original_size);

            if (adjusted_size != original_size) {
                server.hotkey_cms_bucket_size = adjusted_size;
                serverLog(LL_NOTICE, "Hotkey CMS bucket size adjusted from %zu to %zu (nearest power of 2)",
                          original_size, adjusted_size);
            }

            server.hotkey_manager = hotkeyManagerInit(server.hotkey_cms_bucket_size, server.hotkey_cms_depth);
            updateHotkeyCMSThreshold();
        }
    } else {
        if (server.hotkey_manager) {
            hotkeyManagerFree(server.hotkey_manager);
            server.hotkey_manager = NULL;
        }
    }
    return 1;
}

int hotKeyCMSThresholdCallback(const char **err) {
    UNUSED(err);

    /* If hot key detection is not enabled, return directly */
    if (!server.hotkey_enabled) {
        return 1;
    }

    /* Recalculate hot key threshold in CMS */
    updateHotkeyCMSThreshold();

    return 1;
}

/* Hotkey cms bucket size callbacks */
int hotKeyCMSBucketSizeCallback(const char **err) {
    UNUSED(err);

    /* If hot key detection is not enabled, return directly */
    if (!server.hotkey_enabled) {
        return 1;
    }

    /* Automatically adjust to the nearest power of 2 */
    size_t original_size = server.hotkey_cms_bucket_size;
    size_t adjusted_size = nextPowerOfTwo(original_size);

    /* If size was adjusted, update configuration value */
    if (adjusted_size != original_size) {
        server.hotkey_cms_bucket_size = adjusted_size;
        serverLog(LL_NOTICE, "Hotkey CMS bucket size adjusted from %zu to %zu (nearest power of 2)",
                  original_size, adjusted_size);
    }

    /* Recreate CMS to apply new bucket size
    First clean up existing CMS */
    if (server.hotkey_manager) {
        hotkeyManagerFree(server.hotkey_manager);
        server.hotkey_manager = NULL;
    }

    server.hotkey_manager = hotkeyManagerInit(server.hotkey_cms_bucket_size, server.hotkey_cms_depth);
    updateHotkeyCMSThreshold();

    return 1;
}

/* Hotkey cms depth callbacks */
int hotKeyCMSDepthCallback(const char **err) {
    UNUSED(err);

    /* If hot key detection is not enabled, return directly */
    if (!server.hotkey_enabled) {
        return 1;
    }

    /* Recreate CMS to apply new depth
    First clean up existing CMS */
    if (server.hotkey_manager) {
        hotkeyManagerFree(server.hotkey_manager);
        server.hotkey_manager = NULL;
    }

    server.hotkey_manager = hotkeyManagerInit(server.hotkey_cms_bucket_size, server.hotkey_cms_depth);
    updateHotkeyCMSThreshold();

    return 1;
}
