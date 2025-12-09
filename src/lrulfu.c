#include "lrulfu.h"
#include "server.h"

#define LRULFU_MASK ((1 << LRULFU_BITS) - 1) /* Mask for LRU/LFU value */

/**************** LRU ****************/
/* LRU uses a 24 bit timestamp of the last access time (in seconds)
 * The LRU value needs to be "touched" within 194 days, or the value will wrap,
 * and the last access time will appear to be recent.
 */

/* The LRU_CLOCK_RESOLUTION is used to support an older ruby program which tests
 * the LRU behavior.  This should be set to 1 if building Valkey to support this
 * ruby test.  Otherwise, the default of 1000 is expected. */
#define LRU_CLOCK_RESOLUTION 1000 /* LRU clock resolution in ms */


// Current time in seconds (24 least significant bits).  Designed to roll over.
static uint32_t LRUGetClockTime(void) {
#if LRU_CLOCK_RESOLUTION == 1000
    return (uint32_t)(server.unixtime & LRULFU_MASK);
#else
    return (uint32_t)((server.mstime / LRU_CLOCK_RESOLUTION) & LRULFU_MASK);
#endif
}


uint32_t lru_import(uint32_t idle_secs) {
    uint32_t now = LRUGetClockTime();
#if LRU_CLOCK_RESOLUTION != 1000
    idle_secs = (uint32_t)((long)idle_secs * 1000 / LRU_CLOCK_RESOLUTION);
#endif
    idle_secs = idle_secs & LRULFU_MASK;
    // Underflow is ok/expected
    return (now - idle_secs) & LRULFU_MASK;
}


uint32_t lru_getIdleSecs(uint32_t lru) {
    // Underflow is ok/expected
    uint32_t seconds = (LRUGetClockTime() - lru) & LRULFU_MASK;
#if LRU_CLOCK_RESOLUTION != 1000
    seconds = (uint32_t)((long)seconds * LRU_CLOCK_RESOLUTION / 1000);
#endif
    return seconds;
}


/**************** LFU ****************/
/* ----------------------------------------------------------------------------
 * LFU (Least Frequently Used) implementation.
 *
 * We split the 24 bits into two fields:
 *
 *            16 bits           8 bits
 *     +-----------------------+--------+
 *     + Last access (minutes) | LOG_C  |
 *     +-----------------------+--------+
 *
 * LOG_C is a logarithmic counter that provides an indication of the access
 * frequency. However this field must also be decremented otherwise what used
 * to be a frequently accessed key in the past, will remain ranked like that
 * forever, while we want the algorithm to adapt to access pattern changes.
 *
 * So the remaining 16 bits are used in order to store the "access time",
 * a reduced-precision Unix time (we take 16 bits of the time converted
 * in minutes since we don't care about wrapping around) where the LOG_C
 * counter decays every minute by default (depends on lfu-decay-time).
 *
 * New keys don't start at zero, in order to have the ability to collect
 * some accesses before being trashed away, so they start at LFU_INIT_VAL.
 * The logarithmic increment performed on LOG_C takes care of LFU_INIT_VAL
 * when incrementing the key, so that keys starting at LFU_INIT_VAL
 * (or having a smaller value) have a very high chance of being incremented
 * on access. (The chance depends on counter and lfu-log-factor.)
 *
 * During decrement, the value of the logarithmic counter is decremented by
 * one when lfu-decay-time minutes elapsed.
 * --------------------------------------------------------------------------*/

#define LFU_INIT_VAL 5


// Current time in minutes (16 least significant bits).  Designed to roll over.
static uint16_t LFUGetTimeInMinutes(void) {
    return (uint16_t)(server.unixtime / 60);
}


uint32_t lfu_import(uint8_t freq) {
    return ((uint32_t)LFUGetTimeInMinutes() << 8) | freq;
}


/* Update an LFU to consider decay, but doesn't add a "touch" */
static uint32_t LFUDecay(uint32_t lfu) {
    uint16_t now = LFUGetTimeInMinutes();
    uint16_t prev_time = (uint16_t)(lfu >> 8);
    uint8_t freq = (uint8_t)lfu;
    uint16_t elapsed = now - prev_time; // Wrap-around expected/valid
    uint16_t num_periods = server.lfu_decay_time ? elapsed / server.lfu_decay_time : 0;
    freq = (num_periods > freq) ? 0 : freq - num_periods;
    return ((uint32_t)now << 8) | freq;
}


/* Increment the freq counter with logarithmic probability.
 * Values closer to 0 are more likely to increment.
 * Values closer to 255 are logarithmically less likely to increment. */
static uint8_t LFULogIncr(uint8_t freq) {
    if (freq == 255) return freq;
    double r = (double)rand() / RAND_MAX;
    double baseval = (int)freq - LFU_INIT_VAL;
    if (baseval < 0) baseval = 0;
    double p = 1.0 / (baseval * server.lfu_log_factor + 1);
    if (r < p) freq++;
    return freq;
}


uint32_t lfu_touch(uint32_t lfu) {
    lfu = LFUDecay(lfu);
    uint8_t freq = (uint8_t)lfu;
    freq = LFULogIncr(freq);
    return (lfu & ~(uint32_t)UINT8_MAX) | freq;
}


uint32_t lfu_getFrequency(uint32_t lfu, uint8_t *freq) {
    lfu = LFUDecay(lfu);
    *freq = (uint8_t)lfu;
    return lfu;
}


/**************** Generic API ****************/

bool lrulfu_isUsingLFU(void) {
    return (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) != 0;
}


uint32_t lrulfu_init(void) {
    if (lrulfu_isUsingLFU()) {
        return lfu_import(LFU_INIT_VAL);
    } else {
        return lru_import(0);
    }
}


uint32_t lrulfu_getIdleness(uint32_t lrulfu, uint32_t *idleness) {
    if (lrulfu_isUsingLFU()) {
        uint8_t freq;
        lrulfu = lfu_getFrequency(lrulfu, &freq);
        *idleness = UINT8_MAX - freq;
    } else {
        *idleness = lru_getIdleSecs(lrulfu);
    }
    return lrulfu;
}


uint32_t lrulfu_touch(uint32_t lrulfu) {
    if (lrulfu_isUsingLFU()) {
        return lfu_touch(lrulfu);
    } else {
        return lru_import(0);
    }
}
