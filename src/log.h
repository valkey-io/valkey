#ifndef VALKEY_LOG_H
#define VALKEY_LOG_H

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/syslog.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define LOG_MAX_LEN 1024 /* Default maximum length of syslog messages.*/

/* Log levels */
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#define LL_NOTHING 4
#define LL_RAW (1 << 10) /* Modifier to log without timestamp */

/* Use macro for checking log level to avoid evaluating arguments in cases log
 * should be ignored due to low level. */
#define serverLog(level, ...)                                                                                          \
    do {                                                                                                               \
        if (((level) & 0xff) < server.verbosity) break;                                                                \
        valkeyLog(level, server.syslog_enabled, server.timezone, server.daylight_active, server.sentinel_mode,         \
                  server.pid, server.primary_host, server.logfile, __VA_ARGS__);                                       \
    } while (0)

#define serverLogRaw(level, ...)                                                                                       \
    do {                                                                                                               \
        if (((level) & 0xff) < server.verbosity) break;                                                                \
        valkeyLogRaw(level, server.syslog_enabled, server.timezone, server.daylight_active, server.sentinel_mode,      \
                     server.pid, server.primary_host, server.logfile, __VA_ARGS__);                                    \
    } while (0)

#define serverLogFromHandler(level, fmt, ...)                                                                          \
    do {                                                                                                               \
        if (((level) & 0xff) < server.verbosity) break;                                                                \
        valkeyLogFromHandler(level, server.daemonize, server.logfile, fmt, __VA_ARGS__);                               \
    } while (0)

#define serverLogRawFromHandler(level, msg)                                                                            \
    do {                                                                                                               \
        if (((level) & 0xff) < server.verbosity) break;                                                                \
        valkeyLogRawFromHandler(level, server.daemonize, server.logfile, msg);                                         \
    } while (0)

#define serverDebug(fmt, ...) printf("DEBUG %s:%d > " fmt "\n", __FILE__, __LINE__, __VA_ARGS__)
#define serverDebugMark() printf("-- MARK %s:%d --\n", __FILE__, __LINE__)

#ifdef __GNUC__
void valkeyLog(int level,
               int syslog_enabled,
               time_t timezone,
               int daylight_active,
               int sentinel_mode,
               pid_t pid,
               const char *primary_host,
               const char *logfile,
               const char *fmt,
               ...) __attribute__((format(printf, 9, 10)));
void valkeyLogFromHandler(int level, int daemonize, const char *logfile, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
#else
void valkeyLog(int level,
               int syslog_enabled,
               time_t timezone,
               int daylight_active,
               int sentinel_mode,
               pid_t pid,
               const char *primary_host,
               const char *logfile,
               const char *fmt,
               ...);
void valkeyLogFromHandler(int level, int daemonize, const char *logfile, const char *fmt, ...);
#endif
void valkeyLogRawFromHandler(int level, int daemonize, const char *logfile, const char *msg);
void valkeyLogRaw(int level,
                  int syslog_enabled,
                  time_t timezone,
                  int daylight_active,
                  int sentinel_mode,
                  pid_t pid,
                  const char *primary_host,
                  const char *logfile,
                  const char *msg);
#endif
