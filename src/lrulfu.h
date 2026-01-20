#ifndef __LRULFU_H__
#define __LRULFU_H__

/*
 * Important: include fmacros.h before any system header
 * so feature-test macros like _FILE_OFFSET_BITS are in effect
 * when glibc's <features.h> is first seen (transitively via
 * headers like <stdint.h>). This prevents 32-bit off_t on
 * 32-bit builds due to include-order.
 */
#include "fmacros.h"

#include <stdint.h>
#include <stdbool.h>

/* LRU (Least Recently Used) and LFU (Least Frequently Used) numeric logic.
 *
 * Implementation of a 24 bit value which may either be an LRU or LFU value as indicated by the
 * server's maxmemory_policy.
 *
 * LRU - the value consists of a 24-bit time in seconds.  This value will roll over after 194 days.
 *       (If a value is not touched for 194 days, it will appear as recent.)
 *
 * LFU - maintains an approximate logarithmic value indicating the frequency of access.  The first
 *       16 bits maintain the last evaluation time in minutes.  The remaining 8 bits maintain an
 *       approximate frequency of use.  The time value will roll over after 45 days.  If a value
 *       is not evaluated in this time, it may not show as decayed after this time.
 *
 * Returned values are guaranteed to fit in an unsigned 24-bit region.  They can safely be packed
 * like:
 *     struct {
 *         uint32_t lru : LRU_BITS;
 *     }
 */

#define LRULFU_BITS 24

/**************** LRU ****************/

/* Import a given LRU idleness to the current time.  */
uint32_t lru_import(uint32_t idle_secs);

/* Get the current idle secs from the given LRU value.  */
uint32_t lru_getIdleSecs(uint32_t lru);

/**************** LFU ****************/

/* Import a given LFU frequency to the current time.  */
uint32_t lfu_import(uint8_t freq);

/* Update/Touch an LFU value, decays the old value and adds a "touch".  */
uint32_t lfu_touch(uint32_t lfu);

/* Return the LFU frequency, without adding a touch.
 * An updated LFU is returned which maintains the decay on the LFU.  */
uint32_t lfu_getFrequency(uint32_t lfu, uint8_t *freq);


/**************** Generic API ****************/
/* These API functions can be used interchangeably between LRU and LFU, depending on the setting of
 * server.maxmemory_policy.  It is preferred to use these functions rather than directly accessing
 * the LRU/LFU API functions if the use case permits.  Note that if the server's policy is changed,
 * LRU <-> LFU, evaluations will be incorrect until values have had time to be touched/updated.*/

/* Is the server using LFU policy? */
bool lrulfu_isUsingLFU(void);

/* Provide an initial value for LRU or LFU */
uint32_t lrulfu_init(void);

/* Return a relative indication of idleness, used for comparison between LRU or LFU values.
 * A greater number indicates a greater degree of idleness.
 *
 * Returns an updated LRU/LFU value, maintaining the data, without a "touch".  */
uint32_t lrulfu_getIdleness(uint32_t lrulfu, uint32_t *idleness);

/* Add a touch to the LRU or LFU value, returning the updated LRU/LFU.  */
uint32_t lrulfu_touch(uint32_t lrulfu);

#endif
