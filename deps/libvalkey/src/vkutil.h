/*
 * Copyright (c) 2015-2017, Ieshen Zheng <ieshen.zheng at 163 dot com>
 * Copyright (c) 2020, Nick <heronr1 at gmail dot com>
 * Copyright (c) 2020-2021, Bjorn Svensson <bjorn.a.svensson at est dot tech>
 * Copyright (c) 2020-2021, Viktor Söderqvist <viktor.soderqvist at est dot tech>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef VALKEY_VKUTIL_H
#define VALKEY_VKUTIL_H

#include "win32.h"

#include <stddef.h>
#include <stdint.h>

#ifndef _WIN32
#include <sys/time.h>
#endif

/* Static assert macro for C99. */
#define vk_static_assert(cond) extern char vk_static_assert[sizeof(char[(cond) ? 1 : -1])]

/*
 * Wrapper to workaround well known, safe, implicit type conversion when
 * invoking system calls.
 */
#define vk_atoi(_line, _n) _vk_atoi((uint8_t *)_line, (size_t)_n)

int _vk_atoi(uint8_t *line, size_t n);

/* Return the current time in microseconds since Epoch */
static inline int64_t vk_usec_now(void) {
    int64_t usec;
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceCounter(&counter) ||
        !QueryPerformanceFrequency(&frequency)) {
        return -1;
    }
    usec = counter.QuadPart * 1000000 / frequency.QuadPart;
#else
    struct timeval now;
    if (gettimeofday(&now, NULL) < 0) {
        return -1;
    }
    usec = (int64_t)now.tv_sec * 1000000LL + (int64_t)now.tv_usec;
#endif
    return usec;
}

static inline int64_t vk_msec_now(void) {
    return vk_usec_now() / 1000;
}

uint16_t crc16(const char *buf, int len);

static inline int valkeyMin(long long a, long long b) {
    return (a < b) ? a : b;
}

#endif /* VALKEY_VKUTIL_H */
