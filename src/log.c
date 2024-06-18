#include "log.h"

#include "util.h"

#include <fcntl.h>
#include <stdarg.h>
#include <string.h>

/* We use a private localtime implementation which is fork-safe. The logging
 * function of the server may be called from other threads. */
void nolocks_localtime(struct tm *tmp, time_t t, time_t tz, int dst);

/* Low level logging from signal handler. Should be used with pre-formatted strings.
   See serverLogFromHandler. */
void valkeyLogRawFromHandler(int level, int daemonize, const char *logfile, const char *msg) {
    int fd;
    int log_to_stdout = logfile[0] == '\0';
    char buf[64];

    if (log_to_stdout && daemonize) return;
    fd = log_to_stdout ? STDOUT_FILENO : open(logfile, O_APPEND | O_CREAT | O_WRONLY, 0644);
    if (fd == -1) return;
    if (level & LL_RAW) {
        if (write(fd, msg, strlen(msg)) == -1) goto err;
    } else {
        ll2string(buf, sizeof(buf), getpid());
        if (write(fd, buf, strlen(buf)) == -1) goto err;
        if (write(fd, ":signal-handler (", 17) == -1) goto err;
        ll2string(buf, sizeof(buf), time(NULL));
        if (write(fd, buf, strlen(buf)) == -1) goto err;
        if (write(fd, ") ", 2) == -1) goto err;
        if (write(fd, msg, strlen(msg)) == -1) goto err;
        if (write(fd, "\n", 1) == -1) goto err;
    }
err:
    if (!log_to_stdout) close(fd);
}

/* An async-signal-safe version of serverLog. if LL_RAW is not included in level flags,
 * The message format is: <pid>:signal-handler (<time>) <msg> \n
 * with LL_RAW flag only the msg is printed (with no new line at the end)
 *
 * We actually use this only for signals that are not fatal from the point
 * of view of the server. Signals that are going to kill the server anyway and
 * where we need printf-alike features are served by serverLog(). */
void valkeyLogFromHandler(int level, int daemonize, const char *logfile, const char *fmt, ...) {
    va_list ap;
    char msg[LOG_MAX_LEN];

    va_start(ap, fmt);
    vsnprintf_async_signal_safe(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    valkeyLogRawFromHandler(level, daemonize, logfile, msg);
}

/* Low level logging. To use only for very big messages, otherwise
 * valkeyLog() is to prefer. */
void valkeyLogRaw(int level,
                  int syslog_enabled,
                  time_t timezone,
                  int daylight_active,
                  int sentinel_mode,
                  pid_t server_pid,
                  const char *primary_host,
                  const char *logfile,
                  const char *msg) {
    const int syslogLevelMap[] = {LOG_DEBUG, LOG_INFO, LOG_NOTICE, LOG_WARNING};
    const char *c = ".-*#";
    FILE *fp;
    char buf[64];
    int rawmode = (level & LL_RAW);
    int log_to_stdout = logfile[0] == '\0';

    fp = log_to_stdout ? stdout : fopen(logfile, "a");
    if (!fp) return;

    if (rawmode) {
        fprintf(fp, "%s", msg);
    } else {
        int off;
        struct timeval tv;
        int role_char;
        pid_t pid = getpid();

        gettimeofday(&tv, NULL);
        struct tm tm;
        nolocks_localtime(&tm, tv.tv_sec, timezone, daylight_active);
        off = strftime(buf, sizeof(buf), "%d %b %Y %H:%M:%S.", &tm);
        snprintf(buf + off, sizeof(buf) - off, "%03d", (int)tv.tv_usec / 1000);
        if (sentinel_mode) {
            role_char = 'X'; /* Sentinel. */
        } else if (pid != server_pid) {
            role_char = 'C'; /* RDB / AOF writing child. */
        } else {
            role_char = (primary_host ? 'S' : 'M'); /* replica or Primary. */
        }
        fprintf(fp, "%d:%c %s %c %s\n", (int)getpid(), role_char, buf, c[level], msg);
    }
    fflush(fp);

    if (!log_to_stdout) fclose(fp);
    if (syslog_enabled) syslog(syslogLevelMap[level], "%s", msg);
}

/* Like valkeyLogRaw() but with printf-alike support. This is the function that
 * is used across the code. The raw version is only used in order to dump
 * the INFO output on crash. */
void valkeyLog(int level,
               int syslog_enabled,
               time_t timezone,
               int daylight_active,
               int sentinel_mode,
               pid_t server_pid,
               const char *primary_host,
               const char *logfile,
               const char *fmt,
               ...) {
    va_list ap;
    char msg[LOG_MAX_LEN];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    valkeyLogRaw(level, syslog_enabled, timezone, daylight_active, sentinel_mode, server_pid, primary_host, logfile,
                 msg);
}
