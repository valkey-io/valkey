#ifndef __FLASHCACHE_SERIALIZATION_H
#define __FLASHCACHE_SERIALIZATION_H

#include <stdint.h>
#include <stddef.h>

#include "include/log_entry.h"
#include "include/util.h"

#define FC_ITEM_HEADER_VERSION (0)
#define FC_ITEM_HEADER_LEN (sizeof(itemHeader))

/* A simple library for serializing the item written to log/snapshot file. An item is a key/value pair
 * or a replication command (READ/WRITE/DELETE command). Serialized data protocol:
 *
 * +---------+------------------+-----------------+
 * |         |                  |                 |
 * | Header  |       Key        |      Value      |
 * |         |                  |                 |
 * +---------+------------------+-----------------+
 *
 * 1. Header: Header of the entry that contains the length of key and value. It also contains additional
 * flags (which is used to denote different operation types) and checksum.
 * 2. Key: Key of serialized item
 * 3. Value: Value of the serialized item
 */

#define FC_LAST_ITEM_BEFORE_NEXT_PAGE_BOUNDARY      (1LL << 0)
#define FC_SKIP_SEGMENT                             (1LL << 1)
#define FC_REPL_CMD_DELETE                          (1LL << 2)
#define FC_EOF_INDICATOR                            (1LL << 3)

typedef struct {
    // Version of header
    uint32_t version;
    // Flag indicates the type of item. It can be key/value pair(flag: 0), last item before next page boundary(flag: 1),
    // different types of replication command etc
    uint32_t flag;
    // Length of key
    size_t key_len;
    // Length of value
    size_t value_len;
    // Database identifier for the item
    uint32_t dbid;
    // Key field checksum
    uint32_t key_checksum;
    // Value field checksum
    uint32_t value_checksum;
    // Checksum of all the entries in the header except the header checksum
    uint32_t header_checksum;
} itemHeader;

#ifndef __cplusplus
// This ensures that the offset of fields in the itemHeader does not change when we make change in the structure.
// A change in the offset would break the backward compatibility of the log format. This will affect the snapshots
// as well as the same structure is used in snapshot.
_Static_assert(offsetof(itemHeader, version) == 0, "Invalid memory offset for itemHeader:version");
_Static_assert(offsetof(itemHeader, flag) == 4, "Invalid memory offset for itemHeader:flag");
_Static_assert(offsetof(itemHeader, key_len) == 8, "Invalid memory offset for itemHeader:key_len");
_Static_assert(offsetof(itemHeader, value_len) == 16, "Invalid memory offset for itemHeader:value_len");
_Static_assert(offsetof(itemHeader, dbid) == 24, "Invalid memory offset for itemHeader:dbid");
_Static_assert(offsetof(itemHeader, key_checksum) == 28, "Invalid memory offset for itemHeader:key_checksum");
_Static_assert(offsetof(itemHeader, value_checksum) == 32, "Invalid memory offset for itemHeader:value_checksum");
_Static_assert(offsetof(itemHeader, header_checksum) == 36, "Invalid memory offset for itemHeader:header_checksum");
#endif

/* Serializes the key and value into serialized item. This function allocates
 * the memory of the serialized item. The caller is responsible for freeing that
 * memory.
 */
void serializeKeyValuePair(uint32_t dbid, char const *key, size_t key_len, char const *value,
        size_t value_len, char **serialized_item, size_t *serialized_item_len,
        flashcache_crc_function crc_function);

// Updates the flag in the serialized item
void updateFlagInSerializedItem(char *serialized_item, int32_t flag,
        flashcache_crc_function crc_function);

/**
 * Serializes the key/value pair with flag into serialized item. This function is generic enough to support all kinds of
 * replication command like READ/WRITE/DELETE or EOF marker for Snapshot file based on different flags.
 * For read/delete command, value will be NULL and value_len will be 0. This function allocates the memory of the
 * serialized item. The caller is responsible for freeing that memory. Currently it is only being used for Delete
 * replication command.
 */
void serializeKeyValuePairWithFlag(uint32_t dbid, char const *key, size_t key_len, char const *value,
                                   size_t value_len, char **serialized_item, size_t *serialized_item_len,
                                   flashcache_crc_function crc_function, uint32_t flag);

// Writes an item with no key or value. The value length field indicate the number of bytes to skip. This marker
// help skip pages when there is no room to write an item at the end of the log. Skipping the pages help to write
// the item at the start of the log by wrapping around.
void skipSegmentInLogFileMarker(char *buf, size_t bytes_to_skip, flashcache_crc_function crc_function);

// Returns the flag of the serialized item
uint32_t getFlagInSerializedItem(char *serialized_item);

// Returns 1 if the header checksum matches else return 0
int validateHeaderInSerializedItem(char *serialized_item, flashcache_crc_function crc_function);

// Returns 1 if the key checksum matches else return 0
int validateKeyInSerializedItem(char *serialized_item,
        flashcache_crc_function crc_function);

// Returns 1 if the value checksum matches else return 0
int validateValueInSerializedItem(char *serialized_item,
        flashcache_crc_function crc_function);

// Return 1 if the header checksum, key checksum, value checksum matches else returns 0
int validateSerializedItem(char *serialized_item, flashcache_crc_function crc_function);

/* This function compares the provided key length and dbid with the key length and dbid in the
 * serialized item. Returns 1 if the key lengths and dbid match, else returns 0.
 */
int compareKeyLengthAndDbidInSerializedItem(char *serialized_item, uint32_t dbid, size_t key_len);

/* This function compares the provided key and dbid with the key and dbid in the serialized item.
 * Returns 1 if the keys match else returns 0.
 */
int compareKeyAndDbidInSerializedItem(char *serialized_item, uint32_t dbid, char const *key,
        size_t key_len);

/* This method extracts database identifier from the serialized item.
 */
uint32_t extractDbidFromSerializedItem(char *serialized_item);

/* This method extracts value from the serialized item. This function does not allocate memory for value
 * rather return a ptr in the serialized item. The caller should not use value after freeing serialized_item.
 */
void extractValueFromSerializedItem(char *serialized_item, char **value,
        size_t *value_len);

/* This method extracts key from the serialized item. This function does not allocate memory for key
 * rather return a ptr in the serialized item. The caller should not use key after freeing serialized_item.
 */
void extractKeyFromSerializedItem(char *serialized_item, char **key,
        size_t *key_len);

// This method retrieves the total length of the serialized item.
size_t extractTotalLenFromSerializedItem(char *serialized_item);

// This method retrieves the length of the header and key in the serialized item.
size_t extractTotalLenWithoutValueFromSerializedItem(char *serialized_item);

// This method computes the collision hash of the key in the serialized item.
// The provided hash function is used for computing the collision hash.
uint64_t computeCollisionHashOfKeyInSerializedItem(char *serialized_item,
        flashcache_hash_function hash_function);

// Returns size bytes of serialized EOF item
size_t getEOFItemSizeBytes();

#endif  // __FLASHCACHE_SERIALIZATION_H
