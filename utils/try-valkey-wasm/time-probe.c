#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>

int main(void) {
    struct timeval tv;
    struct timespec ts;
    gettimeofday(&tv, NULL);
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t t = time(NULL);
    printf("sizeof(long)=%zu sizeof(time_t)=%zu sizeof(timeval)=%zu\n", sizeof(long), sizeof(time_t), sizeof(tv));
    printf("time()=%lld\n", (long long)t);
    printf("gettimeofday: sec=%lld usec=%lld\n", (long long)tv.tv_sec, (long long)tv.tv_usec);
    printf("clock_gettime: sec=%lld nsec=%lld\n", (long long)ts.tv_sec, (long long)ts.tv_nsec);
    tzset();
    printf("timezone=%ld daylight=%d\n", timezone, daylight);
    struct tm tm;
    localtime_r(&t, &tm);
    printf("localtime: %04d-%02d-%02d %02d:%02d:%02d gmtoff=%ld\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_gmtoff);
    return 0;
}
