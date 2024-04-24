/* These macros are used where the server name is printed in logs and replies.
 * Note the difference in the first letter "V" vs "v". SERVER_TITLE is used in
 * readable text like log messages and SERVER_NAME is used in INFO fields and
 * similar. */
#define SERVER_NAME "valkey"
#define SERVER_TITLE "Valkey"
#define VALKEY_VERSION "255.255.255"
#define VALKEY_VERSION_NUM 0x00ffffff

/* Redis OSS compatibility version, should never
 * exceed 7.2.x. */
#define REDIS_VERSION "7.2.4"
#define REDIS_VERSION_NUM 0x00070204
