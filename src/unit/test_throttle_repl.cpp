/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Unit tests for throttle_repl.h
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "stat_calc.h"
#include "throttle_repl.h"
static monotime fakeGetMonotonicUs(void);
static monotime (*origGetMonotonicUs)(void);
}

static monotime fakeMonotimeUs;

static monotime fakeGetMonotonicUs(void) {
    return fakeMonotimeUs;
}

class ThrottleReplTest : public ::testing::Test {
  protected:
    MockValkey mock;
    RealValkey real;
    static const unsigned long long COB_LIMIT = 10 * 1024 * 1024; /* 10 MB */
    client *replica_steady = nullptr;
    throttler *dummy_throttler = (throttler *)1;

    static void SetUpTestSuite() {
        /* Server set up */
        memset(&server, 0, sizeof(valkeyServer));
        server.hz = CONFIG_DEFAULT_HZ;
        server.replicas = listCreate();
        server.client_obuf_limits[CLIENT_TYPE_REPLICA].soft_limit_bytes = COB_LIMIT;
        server.client_obuf_limits[CLIENT_TYPE_REPLICA].hard_limit_bytes = COB_LIMIT;

        /* throttle_repl set up */
        throttleRepl_config.repl_throttling_enabled = 1;

        /* monotonic set up */
        origGetMonotonicUs = getMonotonicUs;
        getMonotonicUs = fakeGetMonotonicUs;
    }

    static void TearDownTestSuite() {
        getMonotonicUs = origGetMonotonicUs;
        listRelease(server.replicas);
        server.replicas = NULL;
    }

    void SetUp() override {
        replica_steady = createFakeReplicaClient(1);
        replica_steady->repl_data->repl_state = REPLICA_STATE_ONLINE;
        EXPECT_CALL(mock, throttle_getMetrics(_, _)).WillRepeatedly(SetArgPointee<1>(throttleMetrics{}));
        EXPECT_CALL(mock, throttle_getGuardrailSecs(_)).WillRepeatedly(Return(0L));
    }

    void TearDown() override {
        freeFakeReplicaClient(replica_steady);
        replica_steady = NULL;
    }

    client *createFakeReplicaClient(int client_id) {
        client *c = (client *)zcalloc(sizeof(client));
        c->id = client_id;
        c->flag.replica = 1;
        c->repl_data = (ClientReplicationData *)zcalloc(sizeof(ClientReplicationData));
        listAddNodeTail(server.replicas, c);
        return c;
    }

    void freeFakeReplicaClient(client *c) {
        ASSERT_TRUE(c->flag.throttled == 0);
        ASSERT_TRUE(c->throttler == NULL);
        ASSERT_TRUE(c->throttle_node == NULL);
        if (c->cob_trend) trendCalculator_free(c->cob_trend);
        if (c->repl_data) zfree(c->repl_data);
        listNode *ln = listSearchKey(server.replicas, c);
        if (ln) listDelNode(server.replicas, ln);
        zfree(c);
    }

    bool isReplThrottlerActive() {
        return readMetric("repl_throttle_rate") >= 0.0;
    }

    double getThrottlerRate() {
        return readMetric("repl_throttle_rate");
    }

    bool verifyThrottleEvent(long activation_events, long more, long less) {
        return (long)readMetric("repl_throttle_activation_events") == activation_events &&
               (long)readMetric("repl_throttle_more_events") == more &&
               (long)readMetric("repl_throttle_less_events") == less;
    }

  private:
    /* Snapshot the INFO output and return one field's numeric value (-1 if absent). */
    double readMetric(const char *key) {
        sds info = throttleRepl_sdscatInfoMetrics(sdsempty());
        info = throttleRepl_sdscatInfoDebugMetrics(info);
        char search_for[128];
        snprintf(search_for, sizeof(search_for), "%s:", key);
        char *p = strstr(info, search_for);
        double v = p ? strtod(p + strlen(search_for), NULL) : -1.0;
        sdsfree(info);
        return v;
    }
};

TEST_F(ThrottleReplTest, NoReplicasNoThrottle) {
    listEmpty(server.replicas);
    throttleRepl_adjustThrottling();
    EXPECT_FALSE(isReplThrottlerActive());
}

TEST_F(ThrottleReplTest, noCobLimitConfiguredNoThrottle) {
    /* With neither a soft nor a hard COB limit configured, the cob target is 0.
     * The adjustThrottling treats target 0 as "feature off": it never activates, even with
     * a huge COB and increasing trend. */
    server.client_obuf_limits[CLIENT_TYPE_REPLICA].soft_limit_bytes = 0;
    server.client_obuf_limits[CLIENT_TYPE_REPLICA].hard_limit_bytes = 0;

    EXPECT_CALL(mock, getClientOutputBufferMemoryUsage(replica_steady)).WillRepeatedly(Return(COB_LIMIT * 1000));
    EXPECT_CALL(mock, trendCalculator_changePerSecShortTerm(_)).WillRepeatedly(Return(COB_LIMIT * 1000));

    throttleRepl_adjustThrottling();
    EXPECT_FALSE(isReplThrottlerActive());
}

TEST_F(ThrottleReplTest, steadyStateNoThrottleCases) {
    /* Test cases for steady-state replica that throttler will not enabled. */

    /* For cob size < 1/4 soft limit, throttler should not be enabled regardless of trend. */
    /* cob size < 1/4 soft limit, trend is 0 */
    EXPECT_CALL(mock, getClientOutputBufferMemoryUsage(replica_steady)).WillRepeatedly(Return(COB_LIMIT / 8));
    throttleRepl_adjustThrottling();
    EXPECT_FALSE(isReplThrottlerActive());
    EXPECT_TRUE(replica_steady->cob_trend != NULL);

    /* cob size < 1/4 soft limit, huge trend: still no throttle (below threshold, so the trend is not considered) */
    EXPECT_CALL(mock, trendCalculator_changePerSecShortTerm(_)).WillRepeatedly(Return(COB_LIMIT));
    throttleRepl_adjustThrottling();
    EXPECT_FALSE(isReplThrottlerActive());

    /* For cob size >= 1/4 soft limit, throttler should be enabled if the extrapolated cob size exceeds the cob target (1/2 soft limit). */
    /* cob size >= 1/4 soft limit but < 1/2 soft limit, trend is decreasing */
    EXPECT_CALL(mock, getClientOutputBufferMemoryUsage(replica_steady)).WillRepeatedly(Return(COB_LIMIT / 4 + 1));
    EXPECT_CALL(mock, trendCalculator_changePerSecShortTerm(_)).WillRepeatedly(Return(-1.0));
    throttleRepl_adjustThrottling();
    EXPECT_FALSE(isReplThrottlerActive());

    /* cob size >= 1/4 soft limit but < 1/2 soft limit, trend is slowly increasing */
    EXPECT_CALL(mock, getClientOutputBufferMemoryUsage(replica_steady)).WillRepeatedly(Return(COB_LIMIT / 4 + 1));
    EXPECT_CALL(mock, trendCalculator_changePerSecShortTerm(_)).WillRepeatedly(Return(1.0));
    throttleRepl_adjustThrottling();
    EXPECT_FALSE(isReplThrottlerActive());

    /* cob size >1/2 soft limit, trend is decreasing and extrapolated value below target */
    EXPECT_CALL(mock, getClientOutputBufferMemoryUsage(replica_steady)).WillRepeatedly(Return(COB_LIMIT / 2 + 1));
    EXPECT_CALL(mock, trendCalculator_changePerSecShortTerm(_)).WillRepeatedly(Return(-1.0));
    throttleRepl_adjustThrottling();
    EXPECT_FALSE(isReplThrottlerActive());
}

TEST_F(ThrottleReplTest, steadyStateThrottleIncreasingTrend) {
    /* Test case for steady-state replica above threshold (1/4 cob soft limit),
     * for increasing trend, throttle could happen when the extrapolated value exceed the 1/2 cob soft limit. */
    EXPECT_CALL(mock, getClientOutputBufferMemoryUsage(replica_steady)).WillRepeatedly(Return(COB_LIMIT / 4 + 1));
    EXPECT_CALL(mock, trendCalculator_changePerSecShortTerm(_)).WillRepeatedly(Return(COB_LIMIT / 2));

    EXPECT_CALL(mock, throttle_register(_, _, _)).WillOnce(Return(dummy_throttler));
    throttleRepl_adjustThrottling();
    EXPECT_TRUE(isReplThrottlerActive());
    EXPECT_TRUE(verifyThrottleEvent(1, 0, 0)); // Throttler activated, no more/less events yets

    EXPECT_CALL(mock, throttle_adjustRate(_, 0.95)).WillOnce(Return(1.0));
    throttleRepl_adjustThrottling();
    EXPECT_TRUE(verifyThrottleEvent(1, 1, 0)); // Reduce traffic, throttle more traffic.

    EXPECT_CALL(mock, throttle_adjustRate(_, 0.95)).WillOnce(Return(1.0));
    throttleRepl_adjustThrottling();
    EXPECT_TRUE(verifyThrottleEvent(1, 2, 0)); // Reduce traffic, throttles more traffic.

    // Now mock traffic trend is slowed down more, throttler should be deregistered
    EXPECT_CALL(mock, trendCalculator_changePerSecShortTerm(_)).WillRepeatedly(Return(1.0));
    EXPECT_CALL(mock, throttle_adjustRate(_, 1.05)).WillOnce(Return(10000000.0));
    EXPECT_CALL(mock, throttle_deregister(_)).Times(1);
    throttleRepl_adjustThrottling();
    EXPECT_FALSE(isReplThrottlerActive());
    EXPECT_TRUE(verifyThrottleEvent(1, 2, 1));
}

TEST_F(ThrottleReplTest, steadyStateThrottleDecreasingTrend) {
    /* Test case for steady-state replica above threshold (1/4 cob soft limit),
     * for decreasing trend, throttle could happen when the extrapolated value exceed the 1/2 cob soft limit. */
    EXPECT_CALL(mock, getClientOutputBufferMemoryUsage(replica_steady)).WillRepeatedly(Return(COB_LIMIT / 2 + 40));
    EXPECT_CALL(mock, trendCalculator_changePerSecShortTerm(_)).WillRepeatedly(Return(-1.0));

    EXPECT_CALL(mock, throttle_register(_, _, _)).WillOnce(Return(dummy_throttler));
    throttleRepl_adjustThrottling();
    EXPECT_TRUE(isReplThrottlerActive());
    EXPECT_TRUE(verifyThrottleEvent(1, 0, 0)); // Throttler activated, no more/less events yets

    EXPECT_CALL(mock, throttle_adjustRate(_, 0.95)).WillOnce(Return(1.0));
    throttleRepl_adjustThrottling();
    EXPECT_TRUE(verifyThrottleEvent(1, 1, 0)); // Reduce traffic, throttle more traffic.

    EXPECT_CALL(mock, throttle_adjustRate(_, 0.95)).WillOnce(Return(1.0));
    throttleRepl_adjustThrottling();
    EXPECT_TRUE(verifyThrottleEvent(1, 2, 0)); // Reduce traffic, throttles more traffic.

    // Now mock traffic trend is slowed down more, throttler should be deregistered
    EXPECT_CALL(mock, trendCalculator_changePerSecShortTerm(_)).WillRepeatedly(Return(-2.0));
    EXPECT_CALL(mock, throttle_adjustRate(_, 1.05)).WillOnce(Return(10000000.0));
    EXPECT_CALL(mock, throttle_deregister(_)).Times(1);
    throttleRepl_adjustThrottling();
    EXPECT_FALSE(isReplThrottlerActive());
    EXPECT_TRUE(verifyThrottleEvent(1, 2, 1));
}

TEST_F(ThrottleReplTest, steadyStateThrottleBasedOnLargestCob) {
    EXPECT_CALL(mock, getClientOutputBufferMemoryUsage(replica_steady)).WillRepeatedly(Return(COB_LIMIT / 8));
    throttleRepl_adjustThrottling();
    EXPECT_FALSE(isReplThrottlerActive());

    /* Add a second replica with a large COB. The scan tracks the largest
     * COB, so the decision is driven by this replica and throttler activates. */
    client *dummy_replica = createFakeReplicaClient(2);
    dummy_replica->repl_data->repl_state = REPLICA_STATE_ONLINE;
    EXPECT_CALL(mock, getClientOutputBufferMemoryUsage(dummy_replica)).WillRepeatedly(Return(COB_LIMIT));
    EXPECT_CALL(mock, throttle_register(_, _, _)).WillOnce(Return(dummy_throttler));
    throttleRepl_adjustThrottling();
    EXPECT_TRUE(isReplThrottlerActive());
    freeFakeReplicaClient(dummy_replica);
}

TEST_F(ThrottleReplTest, disabledConfigNoNewThrottle) {
    throttleRepl_config.repl_throttling_enabled = 0;
    EXPECT_CALL(mock, getClientOutputBufferMemoryUsage(replica_steady)).WillRepeatedly(Return(COB_LIMIT / 4 + 1));
    EXPECT_CALL(mock, trendCalculator_changePerSecShortTerm(_)).WillRepeatedly(Return(COB_LIMIT / 2));

    throttleRepl_adjustThrottling();

    /* Should not activate when config disabled */
    EXPECT_FALSE(isReplThrottlerActive());
}

TEST_F(ThrottleReplTest, throttlerRemovedAfterFailover) {
    /* Simulate active throttler then failover (become replica) */
    EXPECT_CALL(mock, getClientOutputBufferMemoryUsage(replica_steady)).WillRepeatedly(Return(COB_LIMIT / 4 + 1));
    EXPECT_CALL(mock, trendCalculator_changePerSecShortTerm(_)).WillRepeatedly(Return(COB_LIMIT / 2));
    EXPECT_CALL(mock, throttle_register(_, _, _)).WillOnce(Return(dummy_throttler));
    throttleRepl_adjustThrottling();
    EXPECT_TRUE(isReplThrottlerActive());

    server.primary_host = (char *)"127.0.0.1"; /* now a replica */

    EXPECT_CALL(mock, throttle_deregister(dummy_throttler)).Times(1);
    throttleRepl_adjustThrottling();
    EXPECT_FALSE(isReplThrottlerActive());
}

TEST_F(ThrottleReplTest, clientCobLimitsExempt) {
    // Default should return false since throttler not active
    EXPECT_FALSE(throttleRepl_isClientExemptFromCobLimits(replica_steady));

    /* Throttler now active */
    EXPECT_CALL(mock, getClientOutputBufferMemoryUsage(replica_steady)).WillRepeatedly(Return(COB_LIMIT / 2 + 1));
    EXPECT_CALL(mock, trendCalculator_changePerSecShortTerm(_)).WillRepeatedly(Return(COB_LIMIT / 2));
    EXPECT_CALL(mock, throttle_register(_, _, _)).WillOnce(Return(dummy_throttler));
    throttleRepl_adjustThrottling();
    EXPECT_TRUE(isReplThrottlerActive());

    /* If throttler has been working too long, not exempt. */
    server.unixtime = 1000;
    replica_steady->obuf_soft_limit_reached_time = server.unixtime - 200; // > 4 * STEADY_STATE_CONVERGENCE_SECS
    EXPECT_FALSE(throttleRepl_isClientExemptFromCobLimits(replica_steady));
    replica_steady->obuf_soft_limit_reached_time = server.unixtime - 100; // < 4 * STEADY_STATE_CONVERGENCE_SECS
    EXPECT_TRUE(throttleRepl_isClientExemptFromCobLimits(replica_steady));

    /* If available memory is exhausted, not exempt */
    server.maxmemory = 100;
    EXPECT_CALL(mock, getMaxmemoryState(_, _, _, _)).WillRepeatedly(Return(C_ERR));
    EXPECT_FALSE(throttleRepl_isClientExemptFromCobLimits(replica_steady));
    EXPECT_CALL(mock, getMaxmemoryState(_, _, _, _)).WillRepeatedly(Return(C_OK));
    EXPECT_TRUE(throttleRepl_isClientExemptFromCobLimits(replica_steady));

    /* If the cob size is below the cob target, not exempt */
    EXPECT_CALL(mock, getClientOutputBufferMemoryUsage(replica_steady)).WillRepeatedly(Return(COB_LIMIT / 4));
    EXPECT_FALSE(throttleRepl_isClientExemptFromCobLimits(replica_steady));
    EXPECT_CALL(mock, getClientOutputBufferMemoryUsage(replica_steady)).WillRepeatedly(Return(COB_LIMIT / 2 + 1));
    EXPECT_TRUE(throttleRepl_isClientExemptFromCobLimits(replica_steady));

    /* If it's not replica client, not exempt. */
    replica_steady->flag.replica = 0;
    EXPECT_FALSE(throttleRepl_isClientExemptFromCobLimits(replica_steady));
    replica_steady->flag.replica = 1;
    EXPECT_TRUE(throttleRepl_isClientExemptFromCobLimits(replica_steady));

    /* If I am not primary, not exempt. */
    server.primary_host = (char *)"127.0.0.1";
    EXPECT_FALSE(throttleRepl_isClientExemptFromCobLimits(replica_steady));
    server.primary_host = NULL;
    EXPECT_TRUE(throttleRepl_isClientExemptFromCobLimits(replica_steady));

    /* If throttle repl disabled, not exempt. */
    throttleRepl_config.repl_throttling_enabled = 0;
    EXPECT_FALSE(throttleRepl_isClientExemptFromCobLimits(replica_steady));
    throttleRepl_config.repl_throttling_enabled = 1;
    EXPECT_TRUE(throttleRepl_isClientExemptFromCobLimits(replica_steady));
}
