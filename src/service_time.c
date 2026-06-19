#include "service_time.h"
#include "hdr_histogram.h"

#define SERVICE_TIME_MIN LATENCY_HISTOGRAM_MIN_VALUE
#define SERVICE_TIME_MAX LATENCY_HISTOGRAM_MAX_VALUE
#define SERVICE_TIME_SIGFIGS LATENCY_HISTOGRAM_PRECISION

void serviceTime_startTimer(client *c) {
    if (!server.service_time_tracking_enabled) return;

    if (c->service_time_start == 0) {
        c->service_time_start = aeGetWakeTime(server.el);
    }
}

void serviceTime_trackCmd(client *c) {
    if (!server.service_time_tracking_enabled) return;
    if (c->service_time_start == 0) return;
    if (c->flag.close_after_reply || c->flag.close_asap) {
        serviceTime_reset(c);
        return;
    }
    if (c->flag.reply_off || c->flag.reply_skip) {
        serviceTime_reset(c);
        return;
    }
    if (c->cmd == NULL) return;

    /* Fast path: inline entry is unused or matches. */
    if (c->service_time_entry.count == 0) {
        c->service_time_entry.cmd = c->cmd;
        c->service_time_entry.count = 1;
        return;
    }
    if (c->service_time_entry.cmd == c->cmd) {
        c->service_time_entry.count++;
        return;
    }

    /* Scan pipeline entries for a match. */
    for (int i = 0; i < c->service_time_pipeline_count; i++) {
        if (c->service_time_pipeline[i].cmd == c->cmd) {
            c->service_time_pipeline[i].count++;
            return;
        }
    }

    /* No match — add to pipeline entries if space, otherwise drop. */
    if (c->service_time_pipeline_count < SERVICE_TIME_PIPELINE_CAP) {
        c->service_time_pipeline[c->service_time_pipeline_count].cmd = c->cmd;
        c->service_time_pipeline[c->service_time_pipeline_count].count = 1;
        c->service_time_pipeline_count++;
    }
}

static void updateCommandServiceTimeHistogram(struct hdr_histogram **histogram, int64_t duration_ns) {
    if (*histogram == NULL) {
        hdr_init(SERVICE_TIME_MIN, SERVICE_TIME_MAX, SERVICE_TIME_SIGFIGS, histogram);
    }
    if (duration_ns < SERVICE_TIME_MIN) duration_ns = SERVICE_TIME_MIN;
    if (duration_ns > SERVICE_TIME_MAX) duration_ns = SERVICE_TIME_MAX;
    hdr_record_value(*histogram, duration_ns);
}

void serviceTime_reset(client *c) {
    c->service_time_start = 0;
    c->service_time_entry.cmd = NULL;
    c->service_time_entry.count = 0;
    for (int i = 0; i < c->service_time_pipeline_count; i++) {
        c->service_time_pipeline[i].cmd = NULL;
        c->service_time_pipeline[i].count = 0;
    }
    c->service_time_pipeline_count = 0;
}

void serviceTime_recordLatencies(client *c) {
    if (!server.service_time_tracking_enabled) return;
    if (c->service_time_start == 0 || c->service_time_entry.count == 0) {
        serviceTime_reset(c);
        return;
    }

    monotime now = getMonotonicUs();
    int64_t duration_ns = (int64_t)(now - c->service_time_start) * 1000;

    /* Record inline entry. */
    for (int i = 0; i < c->service_time_entry.count; i++) {
        updateCommandServiceTimeHistogram(&c->service_time_entry.cmd->service_time_histogram, duration_ns);
    }

    /* Record pipeline entries. */
    for (int i = 0; i < c->service_time_pipeline_count; i++) {
        for (int j = 0; j < c->service_time_pipeline[i].count; j++) {
            updateCommandServiceTimeHistogram(&c->service_time_pipeline[i].cmd->service_time_histogram, duration_ns);
        }
    }

    serviceTime_reset(c);
}
