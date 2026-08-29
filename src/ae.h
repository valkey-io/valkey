/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2012, Redis Ltd.
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
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
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

#ifndef __AE_H__
#define __AE_H__

#include "monotonic.h"
#include <pthread.h>

#define AE_OK 0
#define AE_ERR -1

#define AE_NONE 0                      /* No events registered. */
#define AE_READABLE 1                  /* Fire when descriptor is readable. */
#define AE_WRITABLE 2                  /* Fire when descriptor is writable. */
#define AE_BARRIER 4                   /* With WRITABLE, never fire the event if the      \
                                          READABLE event already fired in the same event  \
                                          loop iteration. Useful when you want to persist \
                                          things to disk before sending replies, and want \
                                          to do that in a group fashion. */
#define AE_HIGH_PRIORITY 8             /* Virtual routing mask flag: when set in aeCreateFileEvent(), \
                                        * the event is registered on qos_apidata if available.        \
                                        * Stripped before passing to the underlying OS multiplexer. */
#define AE_QOS_PREEMPT_CHECK_MASK 0x03 /* Mask to check QoS preemption once every 4 iterations */

#define AE_FILE_EVENTS (1 << 0)
#define AE_TIME_EVENTS (1 << 1)
#define AE_ALL_EVENTS (AE_FILE_EVENTS | AE_TIME_EVENTS)
#define AE_DONT_WAIT (1 << 2)
#define AE_CALL_BEFORE_SLEEP (1 << 3)
#define AE_CALL_AFTER_SLEEP (1 << 4)
#define AE_PROTECT_POLL (1 << 5)

#define AE_NOMORE -1
#define AE_DELETED_EVENT_ID -1

/* Macros */
#define AE_NOTUSED(V) ((void)V)

struct timeval; /* forward declaration */
struct aeEventLoop;

/* Opaque per-backend polling state (epoll/kqueue/evport/select).
 *
 * The concrete struct aeApiState is defined privately by each polling backend
 * and its layout varies between them. ae.c only ever holds and passes a typed
 * pointer to it, so the forward declaration here lets the event loop and the
 * inner aeApi* interface use "aeApiState *" instead of an untyped "void *". */
typedef struct aeApiState aeApiState;

/* Types and data structures */
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
typedef long long aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);
typedef void aeAfterSleepProc(struct aeEventLoop *eventLoop, int numevents);
typedef int aeCustomPollProc(struct aeEventLoop *eventLoop);
/* Callback invoked with elapsed microseconds after QoS events are processed. */
typedef void aeQoSStatsProc(struct aeEventLoop *eventLoop, uint64_t duration_us);

/* File event structure */
typedef struct aeFileEvent {
    int mask; /* one of AE_(READABLE|WRITABLE|BARRIER) */
    aeFileProc *rfileProc;
    aeFileProc *wfileProc;
    void *clientData;
} aeFileEvent;

/* Time event structure */
typedef struct aeTimeEvent {
    long long id; /* time event identifier. */
    monotime when;
    aeTimeProc *timeProc;
    aeEventFinalizerProc *finalizerProc;
    void *clientData;
    struct aeTimeEvent *prev;
    struct aeTimeEvent *next;
    int refcount; /* refcount to prevent timer events from being
                   * freed in recursive time event calls. */
} aeTimeEvent;

/* A fired event */
typedef struct aeFiredEvent {
    int fd;
    int mask;
} aeFiredEvent;

/* State of an event based program */
typedef struct aeEventLoop {
    int maxfd;   /* highest file descriptor currently registered */
    int setsize; /* max number of file descriptors tracked */
    long long timeEventNextId;
    aeFileEvent *events; /* Registered events */
    aeFiredEvent *fired; /* Fired events */
    aeTimeEvent *timeEventHead;
    int stop;
    aeApiState *apidata; /* Polling API specific state (owned by the backend) */
    aeBeforeSleepProc *beforesleep;
    aeAfterSleepProc *aftersleep;
    aeCustomPollProc *custompoll;
    pthread_mutex_t poll_mutex;
    int flags;

    /* Quality of Service (QoS):
     * Sockets registered with AE_HIGH_PRIORITY are tracked in qos_apidata.
     * qos_fd is registered into apidata to wake the main loop when QoS traffic arrives.
     * qos_fired holds fired events when draining QoS channels. */
    aeApiState *qos_apidata;                   /* Dedicated QoS polling state */
    int qos_fd;                                /* File descriptor of QoS polling backend (-1 if disabled) */
    aeFiredEvent *qos_fired;                   /* Fired events buffer for QoS polling */
    monotime qos_el_last_poll;                 /* Timestamp when QoS was last drained */
    uint64_t qos_el_preempt_check_interval_us; /* Preemptive check interval in microseconds (0 = disabled) */
    aeQoSStatsProc *qos_el_stats_callback;     /* Callback invoked with elapsed microseconds after draining QoS */
} aeEventLoop;

/* Prototypes */
aeEventLoop *aeCreateEventLoop(int setsize);
void aeDeleteEventLoop(aeEventLoop *eventLoop);
void aeStop(aeEventLoop *eventLoop);
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask, aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
void *aeGetFileClientData(aeEventLoop *eventLoop, int fd);
long long aeCreateTimeEvent(aeEventLoop *eventLoop,
                            long long milliseconds,
                            aeTimeProc *proc,
                            void *clientData,
                            aeEventFinalizerProc *finalizerProc);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);
int aeProcessEvents(aeEventLoop *eventLoop, int flags);
int aeWait(int fd, int mask, long long milliseconds);
void aeMain(aeEventLoop *eventLoop);
char *aeGetApiName(void);
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeAfterSleepProc *aftersleep);
void aeSetCustomPollProc(aeEventLoop *eventLoop, aeCustomPollProc *custompoll);
void aeSetPollProtect(aeEventLoop *eventLoop, int protect);
int aePoll(aeEventLoop *eventLoop, struct timeval *tvp);
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);
void aeSetDontWait(aeEventLoop *eventLoop, int noWait);

/* QoS event loop prototypes */
int aeActuateQoSEventLoopIfSupported(aeEventLoop *eventLoop, uint64_t qosPreemptPollIntervalUs, aeQoSStatsProc *qosStatsCallback);
int aeProcessQoSEventsPreemptively(aeEventLoop *eventLoop);
void aeSetQoSPreemptCheckInterval(aeEventLoop *eventLoop, uint64_t interval_us);

#endif
