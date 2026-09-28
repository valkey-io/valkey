#include <stdlib.h>
#include <string.h>

#include "include/serialization.h"
#include "include/util.h"

#define FC_SERIALIZATION_MAGIC_NUMBER (42)

static inline size_t extractKeyLenFromSerializedItem(char *serialized_item) {
    itemHeader *header = (itemHeader *) serialized_item;
    return header->key_len;
}

static inline size_t getAlignedSizeBytes(size_t size) {
    if (size % FC_ITEM_ALIGNMENT_BYTES == 0) {
        return size;
    }

    return (size / FC_ITEM_ALIGNMENT_BYTES + 1) * FC_ITEM_ALIGNMENT_BYTES;
}

static uint32_t computeHeaderChecksum(itemHeader *header, flashcache_crc_function crc_function) {
    uint32_t prev_header_checksum = header->header_checksum;
    header->header_checksum = 0L;
    uint32_t checksum = crc_function(FC_SERIALIZATION_MAGIC_NUMBER, (char *) header,
            FC_ITEM_HEADER_LEN);
    header->header_checksum = prev_header_checksum;
    return checksum;
}

static void logHeaderDetails(itemHeader *header) {
    flashcacheLogger(FC_LL_WARNING, "Header of the item, version: [%u], flag: [%u], "
            "key len: [%lu], value len: [%lu], dbid: [%u], key checksum: [%u], value checksum: [%u], "
            "header checksum: [%u]", header->version, header->flag, header->key_len, header->value_len,
            header->dbid, header->key_checksum, header->value_checksum, header->header_checksum);
}

void serializeKeyValuePair(uint32_t dbid, char const *key, size_t key_len, char const *value,
        size_t value_len, char **serialized_item, size_t *serialized_item_len,
        flashcache_crc_function crc_function) {
    size_t total_len = getAlignedSizeBytes(FC_ITEM_HEADER_LEN + key_len + value_len);

    char *item = (char *) fcMalloc(total_len);
    flashcacheAssert(item != NULL);

    itemHeader header = { 0 };
    header.version = FC_ITEM_HEADER_VERSION;
    header.dbid = dbid;
    header.key_len = key_len;
    header.value_len = value_len;
    header.key_checksum = crc_function(FC_SERIALIZATION_MAGIC_NUMBER, key, key_len);
    header.value_checksum = crc_function(FC_SERIALIZATION_MAGIC_NUMBER, value, value_len);
    header.header_checksum = computeHeaderChecksum(&header, crc_function);

    size_t pos = 0;
    memcpy(item, &header, FC_ITEM_HEADER_LEN);
    pos += FC_ITEM_HEADER_LEN;

    memcpy(item + FC_ITEM_HEADER_LEN, key, key_len);
    pos += key_len;

    memcpy(item + pos, value, value_len);

    *serialized_item = item;
    *serialized_item_len = total_len;
}

void updateFlagInSerializedItem(char *serialized_item, int32_t flag,
        flashcache_crc_function crc_function) {
    itemHeader *header = (itemHeader *) serialized_item;

    header->flag = flag;
    header->header_checksum = computeHeaderChecksum(header, crc_function);
}

void serializeKeyValuePairWithFlag(uint32_t dbid, char const *key, size_t key_len, char const *value, size_t value_len,
                                   char **serialized_item, size_t *serialized_item_len,
                                   flashcache_crc_function crc_function, uint32_t flag) {
    // Currently we are only using it for Delete replication command and EOF Indicator
    flashcacheAssert(flag == FC_REPL_CMD_DELETE || flag == FC_EOF_INDICATOR);
    serializeKeyValuePair(dbid, key, key_len, value, value_len, serialized_item,
                          serialized_item_len, crc_function);
    updateFlagInSerializedItem(*serialized_item, flag, crc_function);
}

void skipSegmentInLogFileMarker(char *buf, size_t bytes_to_skip, flashcache_crc_function crc_function) {
    flashcacheAssert(bytes_to_skip > sizeof(itemHeader));
    itemHeader header = { 0 };
    header.flag = FC_SKIP_SEGMENT;
    // Substract the item header length from the bytes to avoid skipping extra bytes.
    header.value_len = bytes_to_skip - sizeof(itemHeader);
    header.header_checksum = computeHeaderChecksum(&header, crc_function);
    memcpy(buf, &header, FC_ITEM_HEADER_LEN);
}

uint32_t getFlagInSerializedItem(char *serialized_item) {
    itemHeader *header = (itemHeader *) serialized_item;
    return header->flag;
}

int validateHeaderInSerializedItem(char *serialized_item, flashcache_crc_function crc_function) {
    itemHeader *header = (itemHeader *) serialized_item;

    uint32_t expected_header_checksum = computeHeaderChecksum(header, crc_function);
    int res = (expected_header_checksum == header->header_checksum);
    if (!res) {
        flashcacheLogger(FC_LL_WARNING, "Header checksum mismatch, Received checksum: [%u], "
                "Expected checksum: [%u]", header->header_checksum, expected_header_checksum);
        logHeaderDetails(header);
    }
    return res;
}

int validateKeyInSerializedItem(char *serialized_item, flashcache_crc_function crc_function) {
    itemHeader *header = (itemHeader *) serialized_item;
    size_t pos = FC_ITEM_HEADER_LEN;
    uint32_t key_checksum = crc_function(FC_SERIALIZATION_MAGIC_NUMBER,
            serialized_item + pos, header->key_len);
    int res = (key_checksum == header->key_checksum);
    if (!res) {
        flashcacheLogger(FC_LL_WARNING, "Key checksum mismatch, Received checksum: [%u], "
                "Expected checksum: [%u]", header->key_checksum, key_checksum);
        logHeaderDetails(header);
    }
    return res;
}

int validateValueInSerializedItem(char *serialized_item, flashcache_crc_function crc_function) {
    itemHeader *header = (itemHeader *) serialized_item;
    size_t pos = FC_ITEM_HEADER_LEN + header->key_len;
    uint32_t value_checksum = crc_function(FC_SERIALIZATION_MAGIC_NUMBER,
            serialized_item + pos, header->value_len);
    int res = (value_checksum == header->value_checksum);
    if (!res) {
        flashcacheLogger(FC_LL_WARNING, "Value checksum mismatch, Received checksum: [%u], "
                "Expected checksum: [%u]", header->value_checksum, value_checksum);
        logHeaderDetails(header);
    }
    return res;
}

int validateSerializedItem(char *serialized_item, flashcache_crc_function crc_function) {
    int is_valid = validateHeaderInSerializedItem(serialized_item, crc_function);
    if (!is_valid) {
        return is_valid;
    }

    is_valid = validateKeyInSerializedItem(serialized_item, crc_function);
    if (!is_valid) {
        return is_valid;
    }

    return validateValueInSerializedItem(serialized_item, crc_function);
}

int compareKeyLengthAndDbidInSerializedItem(char *serialized_item, uint32_t dbid, size_t key_len) {
    if (extractDbidFromSerializedItem(serialized_item) != dbid) {
        return 0;
    }

    size_t item_key_len = extractKeyLenFromSerializedItem(serialized_item);
    if (item_key_len != key_len) {
        return 0;
    }

    return 1;
}

int compareKeyAndDbidInSerializedItem(char *serialized_item, uint32_t dbid, char const *key,
        size_t key_len) {
    if (compareKeyLengthAndDbidInSerializedItem(serialized_item, dbid, key_len)) {
        return !memcmp(key, serialized_item + FC_ITEM_HEADER_LEN, key_len);
    }
    return 0;
}

uint32_t extractDbidFromSerializedItem(char *serialized_item) {
    itemHeader *header = (itemHeader *) serialized_item;
    return header->dbid;
}

void extractValueFromSerializedItem(char *serialized_item, char **value,
        size_t *value_len) {
    itemHeader *header = (itemHeader *) serialized_item;
    size_t item_key_len = header->key_len;
    size_t item_value_len = header->value_len;

    char *serialized_value_ptr = serialized_item + FC_ITEM_HEADER_LEN + item_key_len;
    *value = serialized_value_ptr;
    *value_len = item_value_len;
}

void extractKeyFromSerializedItem(char *serialized_item, char **key,
        size_t *key_len) {
    size_t item_key_len = extractKeyLenFromSerializedItem(serialized_item);
    char *serialized_key_ptr = serialized_item + FC_ITEM_HEADER_LEN;
    *key = serialized_key_ptr;
    *key_len = item_key_len;
}

size_t extractTotalLenFromSerializedItem(char *serialized_item) {
    itemHeader *header = (itemHeader *) serialized_item;
    size_t item_key_len = header->key_len;
    size_t item_value_len = header->value_len;
    return getAlignedSizeBytes(FC_ITEM_HEADER_LEN + item_key_len + item_value_len);
}

size_t extractTotalLenWithoutValueFromSerializedItem(char *serialized_item) {
    return FC_ITEM_HEADER_LEN + extractKeyLenFromSerializedItem(serialized_item);
}

uint64_t computeCollisionHashOfKeyInSerializedItem(char *serialized_item,
        flashcache_hash_function hash_function) {
    return computeCollisionHash(hash_function, serialized_item + FC_ITEM_HEADER_LEN,
            extractKeyLenFromSerializedItem(serialized_item));
}

size_t getEOFItemSizeBytes() {
    return getAlignedSizeBytes(FC_ITEM_HEADER_LEN);
}
