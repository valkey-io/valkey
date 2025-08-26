/*
 * RDB Downgrade Compatibility Header
 *
 * This header provides compatibility functions to enable Redis 6.x
 * to load RDB files from newer versions (Redis 7.x RDB v10 and Valkey 8.x RDB
 * v11) that use listpack encoding instead of ziplist encoding.
 */

#ifndef __RDB_DOWNGRADE_COMPAT_H
#define __RDB_DOWNGRADE_COMPAT_H

#include "server.h"
#include "listpack.h"
#include "ziplist.h"
#include "rio.h"
#include <time.h>

/* RDB versions we want to support for downgrade compatibility */
#define RDB_VERSION_REDIS_70 10  /* Redis 7.0 */
#define RDB_VERSION_VALKEY_80 11 /* Redis7.2/Valkey 7.2/8.0+ */

#define QUICKLIST_NODE_CONTAINER_PLAIN 1
#define QUICKLIST_NODE_CONTAINER_PACKED 2

/* Internal constants for safety limits */
#define MAX_LISTPACK_SIZE (1024 * 1024 * 16) /* 16MB max listpack size */
#define MAX_CONVERSION_ELEMENTS                                                \
  65535 /* Max elements in conversion (fits in uint16_t) */
#define LISTPACK_MIN_VALID_SIZE 7 /* Minimum valid listpack size */

/* Listpack to Ziplist conversion functions */
unsigned char *listpackToZiplist(unsigned char *lp);
int isListpackEncoded(unsigned char *data, size_t len);
int quicklistConvertAndValidateIntegrity(unsigned char *lp, size_t size,
                                         unsigned char **zl);

/* Enhanced object loading with compatibility */
robj *rdbLoadObjectCompat(int rdbtype, rio *rdb, sds key, int *error,
                          int rdbver);

/* Load stream with active_time support (RDB v11) */
robj *rdbLoadStreamWithActiveTime(rio *rdb, sds key, int *error, int rdbver);

/* Version compatibility checks */
int requiresListpackConversion(int rdbtype, int rdbver);

/* Production monitoring and statistics */
struct rdbDowngradeStats {
  unsigned long long keys_attempted;
  unsigned long long keys_succeeded;
  unsigned long long keys_failed;
  unsigned long long bytes_converted;
  unsigned long long keys_converted;
  time_t last_conversion_time;
};

extern struct rdbDowngradeStats rdb_downgrade_stats;

/* Statistics and monitoring functions */
void rdbDowngradeStatsUpdateWithType(int success, size_t bytes, const char *key,
                                     int rdbtype);
sds rdbDowngradeStatsInfoString(void);

#endif /* __RDB_DOWNGRADE_COMPAT_H */
