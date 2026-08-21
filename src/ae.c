/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2010, Redis Ltd.
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

#include "ae.h"
#include "anet.h"
#include "serverassert.h"
#include "monotonic.h"

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "zmalloc.h"
#include "config.h"

/* Include the best multiplexing layer supported by this system.
 * The following should be ordered by performances, descending. */
#ifdef HAVE_EVPORT
#include "ae_evport.c"
#else
#ifdef HAVE_EPOLL
#include "ae_epoll.c"
#else
#ifdef HAVE_KQUEUE
#include "ae_kqueue.c"
#else
#include "ae_select.c"
#endif
#endif
#endif

#define AE_LOCK(eventLoop)                                         \
    if ((eventLoop)->flags & AE_PROTECT_POLL) {                    \
        assert(pthread_mutex_lock(&(eventLoop)->poll_mutex) == 0); \
    }

#define AE_UNLOCK(eventLoop)                                         \
    if ((eventLoop)->flags & AE_PROTECT_POLL) {                      \
        assert(pthread_mutex_unlock(&(eventLoop)->poll_mutex) == 0); \
    }

/* Regardless of the flags used in AE, the only flags understood by the backend
 * implementations are AE_READABLE & AE_WRITABLE. */
#define BACKEND_MASK(mask) ((mask) & (AE_READABLE | AE_WRITABLE))

aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;

    monotonicInit(); /* just in case the calling app didn't initialize */

    if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL) goto err;
    eventLoop->events = zmalloc(sizeof(aeFileEvent) * setsize);
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent) * setsize);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;
    eventLoop->setsize = setsize;
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 1;
    eventLoop->stop = 0;
    eventLoop->maxfd = -1;
    eventLoop->beforesleep = NULL;
    eventLoop->aftersleep = NULL;
    eventLoop->custompoll = NULL;
    eventLoop->flags = 0;
    eventLoop->qos_el = NULL;
    eventLoop->qos_el_last_poll_us = 0;
    eventLoop->qos_el_preempt_check_interval_us = 0;
    eventLoop->qos_el_stats_callback = NULL;
    /* Initialize the eventloop mutex with PTHREAD_MUTEX_ERRORCHECK type */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    if (pthread_mutex_init(&eventLoop->poll_mutex, &attr) != 0) goto err;

    if ((eventLoop->apidata = aeApiCreate(setsize)) == NULL) goto err;
    /* Events with mask == AE_NONE are not set. So let's initialize the
     * vector with it. */
    for (i = 0; i < setsize; i++) eventLoop->events[i].mask = AE_NONE;
    return eventLoop;

err:
    if (eventLoop) {
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);
    }
    return NULL;
}

/* Return the current set size. */
int aeGetSetSize(aeEventLoop *eventLoop) {
    return eventLoop->setsize;
}

/*
 * Tell the event processing to change the wait timeout as soon as possible.
 *
 * Note: it just means you turn on/off the global AE_DONT_WAIT.
 */
void aeSetDontWait(aeEventLoop *eventLoop, int noWait) {
    if (noWait)
        eventLoop->flags |= AE_DONT_WAIT;
    else
        eventLoop->flags &= ~AE_DONT_WAIT;
}

/* Resize the maximum set size of the event loop.
 * If the requested set size is smaller than the current set size, but
 * there is already a file descriptor in use that is >= the requested
 * set size minus one, AE_ERR is returned and the operation is not
 * performed at all.
 *
 * Otherwise, AE_OK is returned and the operation is successful. */
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize) {
    AE_LOCK(eventLoop);
    int ret = AE_OK;
    int i;

    if (setsize == eventLoop->setsize) goto done;
    if (eventLoop->maxfd >= setsize) goto err;
    if (aeApiResize(eventLoop->apidata, setsize) == -1) goto err;

    if (eventLoop->qos_el) {
        if (aeResizeSetSize(eventLoop->qos_el, setsize) == AE_ERR) goto err;
    }

    eventLoop->events = zrealloc(eventLoop->events, sizeof(aeFileEvent) * setsize);
    eventLoop->fired = zrealloc(eventLoop->fired, sizeof(aeFiredEvent) * setsize);
    eventLoop->setsize = setsize;

    /* Make sure that if we created new slots, they are initialized with
     * an AE_NONE mask. */
    for (i = eventLoop->maxfd + 1; i < setsize; i++) eventLoop->events[i].mask = AE_NONE;
    goto done;

err:
    ret = AE_ERR;
done:
    AE_UNLOCK(eventLoop);
    return ret;
}

void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    if (eventLoop->qos_el) {
        int qos_fd = aeApiGetPollFd(eventLoop->qos_el);
        if (qos_fd != -1) aeDeleteFileEvent(eventLoop, qos_fd, AE_READABLE);
        aeDeleteEventLoop(eventLoop->qos_el);
    }
    aeApiFree(eventLoop->apidata);
    zfree(eventLoop->events);
    zfree(eventLoop->fired);

    /* Free the time events list. */
    aeTimeEvent *next_te, *te = eventLoop->timeEventHead;
    while (te) {
        next_te = te->next;
        if (te->finalizerProc) te->finalizerProc(eventLoop, te->clientData);
        zfree(te);
        te = next_te;
    }
    zfree(eventLoop);
}

void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}

int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask, aeFileProc *proc, void *clientData) {
    if (mask & AE_HIGH_PRIORITY) {
        if (eventLoop->qos_el) {
            /* Register events on to QoS event loop */
            return aeCreateFileEvent(eventLoop->qos_el, fd, mask & ~AE_HIGH_PRIORITY, proc, clientData);
        }
        /* If qos_el is not initialized, strip the high priority flag */
        mask &= ~AE_HIGH_PRIORITY;
    }
    AE_LOCK(eventLoop);
    int ret = AE_ERR;

    if (fd >= eventLoop->setsize) {
        errno = ERANGE;
        goto done;
    }
    aeFileEvent *fe = &eventLoop->events[fd];

    int backend_add_mask = BACKEND_MASK(mask) & ~fe->mask; // just the meaningful additions
    if (backend_add_mask) {
        if (aeApiAddEvent(eventLoop->apidata, fd, BACKEND_MASK(fe->mask), backend_add_mask) == -1) goto done;
    }
    fe->mask |= mask;
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;
    fe->clientData = clientData;
    if (fd > eventLoop->maxfd) eventLoop->maxfd = fd;

    ret = AE_OK;

done:
    AE_UNLOCK(eventLoop);
    return ret;
}

void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask) {
    if (eventLoop->qos_el) {
        /* Check if the event exists in QoS loop first */
        int qos_mask = aeGetFileEvents(eventLoop->qos_el, fd);
        if (qos_mask != AE_NONE) {
            aeDeleteFileEvent(eventLoop->qos_el, fd, mask);
            return;
        }
    }
    AE_LOCK(eventLoop);
    mask &= ~AE_HIGH_PRIORITY;
    if (fd >= eventLoop->setsize) goto done;

    aeFileEvent *fe = &eventLoop->events[fd];
    if (fe->mask == AE_NONE) goto done;

    /* We want to always remove AE_BARRIER if set when AE_WRITABLE
     * is removed. */
    if (mask & AE_WRITABLE) mask |= AE_BARRIER;

    /* Only remove attached events */
    mask = mask & fe->mask;

    int old_mask = fe->mask;
    fe->mask = fe->mask & ~mask;
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        /* Update the max fd */
        int j;

        for (j = eventLoop->maxfd - 1; j >= 0; j--)
            if (eventLoop->events[j].mask != AE_NONE) break;
        eventLoop->maxfd = j;
    }

    /* Check whether there are events to be removed.
     * Note: user may remove the AE_BARRIER without
     * touching the actual events. */
    int backend_del_mask = BACKEND_MASK(mask); // just the meaningful deletions
    if (backend_del_mask) {
        aeApiDelEvent(eventLoop->apidata, fd, BACKEND_MASK(old_mask), backend_del_mask);
    }

done:
    AE_UNLOCK(eventLoop);
}

void *aeGetFileClientData(aeEventLoop *eventLoop, int fd) {
    if (eventLoop->qos_el) {
        int mask = aeGetFileEvents(eventLoop->qos_el, fd);
        if (mask != AE_NONE) {
            return aeGetFileClientData(eventLoop->qos_el, fd);
        }
    }
    if (fd >= eventLoop->setsize) return NULL;
    aeFileEvent *fe = &eventLoop->events[fd];
    if (fe->mask == AE_NONE) return NULL;

    return fe->clientData;
}

int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
    if (eventLoop->qos_el) {
        int mask = aeGetFileEvents(eventLoop->qos_el, fd);
        if (mask != AE_NONE) {
            return mask | AE_HIGH_PRIORITY;
        }
    }
    if (fd >= eventLoop->setsize) return 0;
    aeFileEvent *fe = &eventLoop->events[fd];

    return fe->mask;
}

long long aeCreateTimeEvent(aeEventLoop *eventLoop,
                            long long milliseconds,
                            aeTimeProc *proc,
                            void *clientData,
                            aeEventFinalizerProc *finalizerProc) {
    long long id = eventLoop->timeEventNextId++;
    aeTimeEvent *te;

    te = zmalloc(sizeof(*te));
    if (te == NULL) return AE_ERR;
    te->id = id;
    te->when = getMonotonicUs() + milliseconds * 1000;
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
    te->clientData = clientData;
    te->prev = NULL;
    te->next = eventLoop->timeEventHead;
    te->refcount = 0;
    if (te->next) te->next->prev = te;
    eventLoop->timeEventHead = te;
    return id;
}

int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id) {
    aeTimeEvent *te = eventLoop->timeEventHead;
    while (te) {
        if (te->id == id) {
            te->id = AE_DELETED_EVENT_ID;
            return AE_OK;
        }
        te = te->next;
    }
    return AE_ERR; /* NO event with the specified ID found */
}

/* How many microseconds until the first timer should fire.
 * If there are no timers, -1 is returned.
 *
 * Note that's O(N) since time events are unsorted.
 * Possible optimizations (not needed so far, but...):
 * 1) Insert the event in order, so that the nearest is just the head.
 *    Much better but still insertion or deletion of timers is O(N).
 * 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
 */
static int64_t usUntilEarliestTimer(aeEventLoop *eventLoop) {
    aeTimeEvent *te = eventLoop->timeEventHead;
    if (te == NULL) return -1;

    aeTimeEvent *earliest = NULL;
    while (te) {
        if ((!earliest || te->when < earliest->when) && te->id != AE_DELETED_EVENT_ID) earliest = te;
        te = te->next;
    }

    /* All events are pending deletion (zombies only): no valid timer. */
    if (earliest == NULL) return -1;

    monotime now = getMonotonicUs();
    return (now >= earliest->when) ? 0 : earliest->when - now;
}

/* Process time events */
static int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te;
    long long maxId;

    te = eventLoop->timeEventHead;
    maxId = eventLoop->timeEventNextId - 1;
    monotime now = getMonotonicUs();
    while (te) {
        long long id;

        /* Remove events scheduled for deletion. */
        if (te->id == AE_DELETED_EVENT_ID) {
            aeTimeEvent *next = te->next;
            /* If a reference exists for this timer event,
             * don't free it. This is currently incremented
             * for recursive timerProc calls */
            if (te->refcount) {
                te = next;
                continue;
            }
            if (te->prev)
                te->prev->next = te->next;
            else
                eventLoop->timeEventHead = te->next;
            if (te->next) te->next->prev = te->prev;
            if (te->finalizerProc) {
                te->finalizerProc(eventLoop, te->clientData);
                now = getMonotonicUs();
            }
            zfree(te);
            te = next;
            continue;
        }

        /* Make sure we don't process time events created by time events in
         * this iteration. Note that this check is currently useless: we always
         * add new timers on the head, however if we change the implementation
         * detail, this check may be useful again: we keep it here for future
         * defense. */
        if (te->id > maxId) {
            te = te->next;
            continue;
        }

        if (te->when <= now) {
            long long retval;

            id = te->id;
            te->refcount++;
            retval = te->timeProc(eventLoop, id, te->clientData);
            te->refcount--;
            processed++;
            now = getMonotonicUs();
            if (retval != AE_NOMORE) {
                te->when = now + (monotime)retval * 1000;
            } else {
                te->id = AE_DELETED_EVENT_ID;
            }
        }
        te = te->next;
    }
    return processed;
}

/* This function provides direct access to the aeApiPoll call.
 * It is intended to be called from a custom poll function.*/
int aePoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    AE_LOCK(eventLoop);

    int ret = aeApiPoll(eventLoop->apidata, eventLoop->fired, eventLoop->events, eventLoop->setsize, eventLoop->maxfd, tvp);

    AE_UNLOCK(eventLoop);
    return ret;
}

/* Process all QoS events and invoke the stats callback
 * with the elapsed time in microseconds if registered. */
static int aeProcessQoSEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    if (eventLoop->qos_el != NULL) {
        monotime start = getMonotonicUs();
        processed = aeProcessEvents(eventLoop->qos_el, AE_ALL_EVENTS | AE_DONT_WAIT);
        eventLoop->qos_el_last_poll_us = getMonotonicUs();
        /* Update INFO stats if registered and QoS events were processed */
        if (eventLoop->qos_el_stats_callback != NULL && processed > 0) {
            eventLoop->qos_el_stats_callback(eventLoop, eventLoop->qos_el_last_poll_us - start);
        }
    }
    return processed;
}

/* Set callback to receive elapsed duration of QoS event processing */
static void aeSetQoSStatsCallback(aeEventLoop *eventLoop, aeQoSStatsProc *cb) {
    eventLoop->qos_el_stats_callback = cb;
}

/* Set the preemptive poll interval in microseconds for QoS events.
 * QoS events are checked periodically during normal event processing
 * when processing batches of normal events exceeds this interval.
 * Setting this interval to 0 disables preemptive polling. */
void aeSetQoSPreemptCheckInterval(aeEventLoop *eventLoop, uint64_t interval_us) {
    eventLoop->qos_el_preempt_check_interval_us = interval_us;
}

/* Preemptively processes QoS events if the elapsed time since the last poll
 * exceeds the qos_el_preempt_check_interval_us threshold (0 disables preemption). */
int aeProcessQoSEventsPreemptively(aeEventLoop *eventLoop) {
    /* Skip if the multiplexer backend doesn't support nested event loop or preemptive polling is disabled */
    if (eventLoop->qos_el == NULL || eventLoop->qos_el_preempt_check_interval_us == 0) return 0;
    if (elapsedUs(eventLoop->qos_el_last_poll_us) < eventLoop->qos_el_preempt_check_interval_us) return 0;
    return aeProcessQoSEvents(eventLoop);
}

/* Process every pending file event, then every pending time event
 * (that may be registered by file event callbacks just processed).
 * Without special flags the function sleeps until some file event
 * fires, or when the next time event occurs (if any).
 *
 * If flags is 0, the function does nothing and returns.
 * if flags has AE_ALL_EVENTS set, all the kind of events are processed.
 * if flags has AE_FILE_EVENTS set, file events are processed.
 * if flags has AE_TIME_EVENTS set, time events are processed.
 * if flags has AE_DONT_WAIT set, the function returns ASAP once all
 * the events that can be handled without a wait are processed.
 * if flags has AE_CALL_AFTER_SLEEP set, the aftersleep callback is called.
 * if flags has AE_CALL_BEFORE_SLEEP set, the beforesleep callback is called.
 *
 * The function returns the number of events processed. */
int aeProcessEvents(aeEventLoop *eventLoop, int flags) {
    int processed = 0, numevents;

    /* Nothing to do? return ASAP */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    /* Note that we want to call aeApiPoll() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. */
    if (eventLoop->maxfd != -1 || ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        int j;
        struct timeval tv, *tvp = NULL; /* NULL means infinite wait. */
        int64_t usUntilTimer;

        if (eventLoop->beforesleep != NULL && (flags & AE_CALL_BEFORE_SLEEP)) eventLoop->beforesleep(eventLoop);

        if (eventLoop->custompoll != NULL) {
            numevents = eventLoop->custompoll(eventLoop);
        } else {
            /* The eventLoop->flags may be changed inside beforesleep.
             * So we should check it after beforesleep be called. At the same time,
             * the parameter flags always should have the highest priority.
             * That is to say, once the parameter flag is set to AE_DONT_WAIT,
             * no matter what value eventLoop->flags is set to, we should ignore it. */
            if ((flags & AE_DONT_WAIT) || (eventLoop->flags & AE_DONT_WAIT)) {
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else if (flags & AE_TIME_EVENTS) {
                usUntilTimer = usUntilEarliestTimer(eventLoop);
                if (usUntilTimer >= 0) {
                    tv.tv_sec = usUntilTimer / 1000000;
                    tv.tv_usec = usUntilTimer % 1000000;
                    tvp = &tv;
                }
            }
            /* Call the multiplexing API, will return only on timeout or when
             * some event fires. */
            numevents = aeApiPoll(eventLoop->apidata, eventLoop->fired, eventLoop->events, eventLoop->setsize, eventLoop->maxfd, tvp);
        }

        /* Don't process file events if not requested. */
        if (!(flags & AE_FILE_EVENTS)) {
            numevents = 0;
        }

        /* After sleep callback. */
        if (eventLoop->aftersleep != NULL && flags & AE_CALL_AFTER_SLEEP) eventLoop->aftersleep(eventLoop, numevents);

        /* Prioritize QoS event loop: if qos_fd fired, drain QoS events
         * immediately before processing normal events. */
        int qos_fd = -1;
        if (eventLoop->qos_el != NULL) {
            qos_fd = aeApiGetPollFd(eventLoop->qos_el);
            if (qos_fd != -1) {
                for (j = 0; j < numevents; j++) {
                    if (eventLoop->fired[j].fd == qos_fd) {
                        processed += aeProcessQoSEvents(eventLoop);
                        break;
                    }
                }
            }
        }

        for (j = 0; j < numevents; j++) {
            /* Periodic Preemptive Poll:
             * If processing normal events is taking more than qos_el_preempt_check_interval_us, check for new QoS events. */
            processed += aeProcessQoSEventsPreemptively(eventLoop);

            int fd = eventLoop->fired[j].fd;
            /* Skip processing QoS events here again as they were already processed above */
            if (fd == qos_fd) continue;
            aeFileEvent *fe = &eventLoop->events[fd];
            int mask = eventLoop->fired[j].mask;
            int fired = 0; /* Number of events fired for current fd. */

            /* Normally we execute the readable event first, and the writable
             * event later. This is useful as sometimes we may be able
             * to serve the reply of a query immediately after processing the
             * query.
             *
             * However if AE_BARRIER is set in the mask, our application is
             * asking us to do the reverse: never fire the writable event
             * after the readable. In such a case, we invert the calls.
             * This is useful when, for instance, we want to do things
             * in the beforeSleep() hook, like fsyncing a file to disk,
             * before replying to a client. */
            int invert = fe->mask & AE_BARRIER;

            /* Note the "fe->mask & mask & ..." code: maybe an already
             * processed event removed an element that fired and we still
             * didn't processed, so we check if the event is still valid.
             *
             * Fire the readable event if the call sequence is not
             * inverted. */
            if (!invert && fe->mask & mask & AE_READABLE) {
                fe->rfileProc(eventLoop, fd, fe->clientData, mask);
                fired++;
                fe = &eventLoop->events[fd]; /* Refresh in case of resize. */
            }

            /* Fire the writable event. */
            if (fe->mask & mask & AE_WRITABLE) {
                if (!fired || fe->wfileProc != fe->rfileProc) {
                    fe->wfileProc(eventLoop, fd, fe->clientData, mask);
                    fired++;
                }
            }

            /* If we have to invert the call, fire the readable event now
             * after the writable one. */
            if (invert) {
                fe = &eventLoop->events[fd]; /* Refresh in case of resize. */
                if ((fe->mask & mask & AE_READABLE) && (!fired || fe->wfileProc != fe->rfileProc)) {
                    fe->rfileProc(eventLoop, fd, fe->clientData, mask);
                    fired++;
                }
            }

            processed++;
        }
    }
    /* Check time events */
    if (flags & AE_TIME_EVENTS) processed += processTimeEvents(eventLoop);

    return processed; /* return the number of processed file/time events */
}

/* Wait for milliseconds until the given file descriptor becomes
 * writable/readable/exception */
int aeWait(int fd, int mask, long long milliseconds) {
    struct pollfd pfd;
    int retmask = 0, retval;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    if (mask & AE_READABLE) pfd.events |= POLLIN;
    if (mask & AE_WRITABLE) pfd.events |= POLLOUT;

    if ((retval = poll(&pfd, 1, milliseconds)) == 1) {
        if (pfd.revents & POLLIN) retmask |= AE_READABLE;
        if (pfd.revents & POLLOUT) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLERR) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLHUP) retmask |= AE_WRITABLE;
        return retmask;
    } else {
        return retval;
    }
}

void aeMain(aeEventLoop *eventLoop) {
    eventLoop->stop = 0;
    while (!eventLoop->stop) {
        aeProcessEvents(eventLoop, AE_ALL_EVENTS | AE_CALL_BEFORE_SLEEP | AE_CALL_AFTER_SLEEP);
    }
}

char *aeGetApiName(void) {
    return aeApiName();
}

void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}

void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeAfterSleepProc *aftersleep) {
    eventLoop->aftersleep = aftersleep;
}

/* This function allows setting a custom poll procedure to be used by the event loop.
 * The custom poll procedure, if set, will be called instead of the default aeApiPoll */
void aeSetCustomPollProc(aeEventLoop *eventLoop, aeCustomPollProc *custompoll) {
    eventLoop->custompoll = custompoll;
}

void aeSetPollProtect(aeEventLoop *eventLoop, int protect) {
    if (protect) {
        eventLoop->flags |= AE_PROTECT_POLL;
    } else {
        eventLoop->flags &= ~AE_PROTECT_POLL;
    }
}

/* Register a QoS event loop file descriptor in the main event loop,
 * which will awake main eventloop when the QoS event loop file descriptor is fired. */
static int aeLinkQoSEventLoop(aeEventLoop *eventLoop, aeEventLoop *qos_el) {
    assert(eventLoop != NULL && qos_el != NULL);
    int qos_fd = aeApiGetPollFd(qos_el);
    if (qos_fd == -1) return AE_ERR;
    /* Register qos_fd with NULL callback: qos_fd is only used to wake up the main loop's
     * multiplexer when QoS traffic arrives. aeProcessEvents() checks for qos_fd and drains
     * qos_el directly before dispatching normal events, and explicitly skips callback
     * dispatch for qos_fd. */
    if (aeCreateFileEvent(eventLoop, qos_fd, AE_READABLE, NULL, NULL) == AE_ERR) {
        return AE_ERR;
    }
    eventLoop->qos_el = qos_el;
    return AE_OK;
}
/* Actuate QoS event loop if supported: creates a nested eventloop for internal
 * connections (cluster bus, replication, and slot migration jobs). It installs
 * a preemptive polling mechanism that wakes the main event loop and processes
 * QoS events at regular interval. If qosPreemptPollIntervalUs is 0, standard
 * event loop behavior is preserved. The provided stats callback is invoked with the
 * elapsed duration of each QoS event processing cycle, enabling monitoring and
 * statistics for QoS processing.
 * Returns AE_OK on success, or AE_ERR if QoS eventloop actuation fails
 * (e.g., unsupported platform, memory allocation failure, or I/O error).
 */
int aeActuateQoSEventLoopIfSupported(aeEventLoop *eventLoop, int setsize, uint64_t qosPreemptPollIntervalUs, aeQoSStatsProc *qosStatsCallback) {
    assert(eventLoop != NULL);

    /* Create QoS event loop for internal connections (cluster bus, replication, and slot migration jobs) */
    aeEventLoop *qos_el = aeCreateEventLoop(setsize);
    if (qos_el == NULL) return AE_ERR;

    /* Set the QoS event preemption check interval in microseconds. */
    aeSetQoSPreemptCheckInterval(eventLoop, qosPreemptPollIntervalUs);

    /* Sets the stats callback invoked with elapsed duration whenever QoS events are processed. */
    aeSetQoSStatsCallback(eventLoop, qosStatsCallback);

    /* Link QoS eventloop with main eventloop via nested multiplexer polling.
     * If multiplexer nesting is unsupported (e.g. evport, select), gracefully
     * free QoS eventloop and fall back to main eventloop event processing without QoS. */
    if (aeLinkQoSEventLoop(eventLoop, qos_el) == AE_ERR) {
        /* gracefully free QoS eventloop and fall back to main eventloop event processing without QoS */
        aeDeleteEventLoop(qos_el);
        return AE_ERR;
    }
    return AE_OK;
}
