/*
 * Payload histogram command implementation.
 */

#include "payload_histogram.h"

#include "hdr_histogram.h"
#include <limits.h>

static void fillPayloadCDF(client *c, struct hdr_histogram *histogram, int64_t base, int64_t factor) {
    addReplyMapLen(c, 2);
    addReplyBulkCString(c, "calls");
    addReplyLongLong(c, (long long)histogram->total_count);
    addReplyBulkCString(c, "histogram_bytes");
    void *replylen = addReplyDeferredLen(c);
    int samples = 0;
    struct hdr_iter iter;
    hdr_iter_log_init(&iter, histogram, base, factor);
    int64_t previous_count = 0;
    while (hdr_iter_next(&iter)) {
        const int64_t bytes = iter.highest_equivalent_value;
        const int64_t cumulative_count = iter.cumulative_count;
        const int64_t bucket_count = cumulative_count - previous_count;
        if (bucket_count > 0) {
            addReplyLongLong(c, (long long)bytes);
            addReplyLongLong(c, (long long)bucket_count);
            samples++;
        }
        previous_count = cumulative_count;
    }
    setDeferredMapLen(c, replylen, samples);
}

static void fillPayloadViews(client *c, struct hdr_histogram *histogram) {
    void *replylen = addReplyDeferredLen(c);
    int views = 0;
    if (histogram && histogram->total_count > 0) {
        for (int j = 0; j < server.payload_histogram_views_len; j++) {
            size_t base = server.payload_histogram_views[j];
            addReplyLongLong(c, (long long)base);
            fillPayloadCDF(c, histogram, base, server.payload_histogram_factor);
            views++;
        }
    }
    setDeferredMapLen(c, replylen, views);
}

static void payloadHistogramCommand(client *c) {
    int direction = 0;
    if (c->argc == 2) {
        direction = 3;
    } else if (c->argc == 3) {
        if (!strcasecmp(objectGetVal(c->argv[2]), "reset")) {
            if (server.payload_read_histogram) hdr_reset(server.payload_read_histogram);
            if (server.payload_write_histogram) hdr_reset(server.payload_write_histogram);
            server.payload_tracking_sample_counter = 0;
            addReply(c, shared.ok);
            return;
        }
        if (!strcasecmp(objectGetVal(c->argv[2]), "read")) {
            direction = 1;
        } else if (!strcasecmp(objectGetVal(c->argv[2]), "write")) {
            direction = 2;
        } else {
            addReplySubcommandSyntaxError(c);
            return;
        }
    } else {
        addReplySubcommandSyntaxError(c);
        return;
    }

    addReplyMapLen(c, (direction == 3) ? 2 : 1);
    if (direction & 1) {
        addReplyBulkCString(c, "read");
        fillPayloadViews(c, server.payload_read_histogram);
    }
    if (direction & 2) {
        addReplyBulkCString(c, "write");
        fillPayloadViews(c, server.payload_write_histogram);
    }
}

static const char *payloadTrackingModeString(int mode) {
    if (mode == PAYLOAD_TRACKING_MODE_READ) return "read";
    if (mode == PAYLOAD_TRACKING_MODE_WRITE) return "write";
    return "both";
}

static int payloadTrackingParseMode(const char *arg, int *mode) {
    if (!strcasecmp(arg, "read")) {
        *mode = PAYLOAD_TRACKING_MODE_READ;
        return 1;
    }
    if (!strcasecmp(arg, "write")) {
        *mode = PAYLOAD_TRACKING_MODE_WRITE;
        return 1;
    }
    if (!strcasecmp(arg, "both")) {
        *mode = PAYLOAD_TRACKING_MODE_BOTH;
        return 1;
    }
    return 0;
}

static void payloadTrackingCommand(client *c) {
    if (c->argc < 3) {
        addReplySubcommandSyntaxError(c);
        return;
    }

    if (!strcasecmp(objectGetVal(c->argv[2]), "on")) {
        long long seconds = 60;
        long long sample_rate = 1;
        int mode = PAYLOAD_TRACKING_MODE_BOTH;
        int seen_seconds = 0, seen_sample = 0, seen_mode = 0;

        for (int j = 3; j < c->argc; j++) {
            const char *arg = objectGetVal(c->argv[j]);
            if (!strcasecmp(arg, "sample")) {
                long long rate = 0;
                if (seen_sample || j + 1 >= c->argc) {
                    addReplySubcommandSyntaxError(c);
                    return;
                }
                if (getLongLongFromObjectOrReply(c, c->argv[j + 1], &rate, NULL) != C_OK) return;
                if (rate < 1) {
                    addReplyError(c, "SAMPLE rate must be >= 1");
                    return;
                }
                sample_rate = rate;
                seen_sample = 1;
                j++;
                continue;
            }

            if (payloadTrackingParseMode(arg, &mode)) {
                if (seen_mode) {
                    addReplySubcommandSyntaxError(c);
                    return;
                }
                seen_mode = 1;
                continue;
            }

            if (!seen_seconds) {
                long long secs = 0;
                if (getLongLongFromObjectOrReply(c, c->argv[j], &secs, NULL) != C_OK) return;
                if (secs <= 0 || secs > LLONG_MAX / 1000) {
                    addReplyError(c, "Seconds must be a positive integer");
                    return;
                }
                seconds = secs;
                seen_seconds = 1;
                continue;
            }

            addReplySubcommandSyntaxError(c);
            return;
        }

        server.payload_tracking_enabled = 1;
        server.payload_tracking_disable_at = server.mstime + (mstime_t)seconds * 1000;
        server.payload_tracking_sample_rate = sample_rate;
        server.payload_tracking_mode = mode;
        server.payload_tracking_sample_counter = 0;
        addReply(c, shared.ok);
        return;
    }

    if (!strcasecmp(objectGetVal(c->argv[2]), "off")) {
        server.payload_tracking_enabled = 0;
        server.payload_tracking_disable_at = 0;
        addReply(c, shared.ok);
        return;
    }

    if (!strcasecmp(objectGetVal(c->argv[2]), "status")) {
        long long remaining = 0;
        if (server.payload_tracking_enabled && server.payload_tracking_disable_at > server.mstime) {
            remaining = (server.payload_tracking_disable_at - server.mstime + 999) / 1000;
        }
        addReplyMapLen(c, 4);
        addReplyBulkCString(c, "enabled");
        addReplyLongLong(c, server.payload_tracking_enabled ? 1 : 0);
        addReplyBulkCString(c, "remaining_seconds");
        addReplyLongLong(c, remaining);
        addReplyBulkCString(c, "sample_rate");
        addReplyLongLong(c, server.payload_tracking_sample_rate);
        addReplyBulkCString(c, "mode");
        addReplyBulkCString(c, payloadTrackingModeString(server.payload_tracking_mode));
        return;
    }

    addReplySubcommandSyntaxError(c);
}

void payloadCommand(client *c) {
    if (!strcasecmp(objectGetVal(c->argv[1]), "histogram")) {
        payloadHistogramCommand(c);
    } else if (!strcasecmp(objectGetVal(c->argv[1]), "tracking")) {
        payloadTrackingCommand(c);
    } else if (!strcasecmp(objectGetVal(c->argv[1]), "help") && c->argc == 2) {
        const char *help[] = {
            "HISTOGRAM [read|write|reset]",
            "    Return a per-bucket distribution of payload sizes for client commands.",
            "    Without an argument, return both read and write histograms.",
            "HISTOGRAM RESET",
            "    Reset payload histograms and sample counters.",
            "TRACKING ON [seconds] [SAMPLE <rate>] [READ|WRITE|BOTH]",
            "    Enable payload tracking and auto-disable after the specified seconds (default 60).",
            "TRACKING OFF",
            "    Disable payload tracking immediately.",
            "TRACKING STATUS",
            "    Return payload tracking status.",
            NULL,
        };
        addReplyHelp(c, help);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}
