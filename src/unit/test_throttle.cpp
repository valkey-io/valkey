/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Unit tests for throttle.h.
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "throttle.h"
static monotime fakeGetMonotonicUs(void);
static monotime (*origGetMonotonicUs)(void);

static bool fakeWriteCriteria(client *c, void *priv_data) {
    UNUSED(priv_data);
    return c->cmd && (c->cmd->flags & CMD_WRITE);
}
}

static monotime fakeMonotimeUs;

static monotime fakeGetMonotonicUs(void) {
    return fakeMonotimeUs;
}

class ThrottleTest : public ::testing::Test {
  protected:
    MockValkey mock;
    RealValkey real;
    serverCommand get_cmd;
    serverCommand set_cmd;
    static inline ConnectionType dummyConnType = {0};

    static void SetUpTestSuite() {
        memset(&server, 0, sizeof(valkeyServer));
        server.hz = CONFIG_DEFAULT_HZ;
        dummyConnType.set_read_handler = dummySetReadHandler;
        throttle_init();

        origGetMonotonicUs = getMonotonicUs;
        getMonotonicUs = fakeGetMonotonicUs;
    }

    static void TearDownTestSuite() {
        getMonotonicUs = origGetMonotonicUs;
    }

    void SetUp() override {
        fakeMonotimeUs = 100;
        get_cmd = {0};
        get_cmd.fullname = (sds) "get";
        get_cmd.proc = getCommand;
        get_cmd.flags = CMD_READONLY;

        set_cmd = {0};
        set_cmd.fullname = (sds) "set";
        set_cmd.proc = setCommand;
        set_cmd.flags = CMD_WRITE;
    }

    void TearDown() override {
    }

    static int dummySetReadHandler(connection *conn, ConnectionCallbackFunc func) {
        conn->read_handler = func;
        return C_OK;
    }

    client *createFakeClient(int client_id, bool write_command) {
        client *c = (client *)zcalloc(sizeof(client));
        c->id = client_id;
        c->conn = (connection *)zcalloc(sizeof(connection));
        c->conn->type = &dummyConnType;
        c->conn->read_handler = (ConnectionCallbackFunc)1;
        c->flag.pending_command = 1;
        c->cmd = write_command ? &set_cmd : &get_cmd;
        return c;
    }

    void freeFakeClient(client *c) {
        EXPECT_EQ(c->throttler, nullptr);
        EXPECT_EQ(c->throttle_node, nullptr);
        EXPECT_EQ(c->flag.throttled, 0ULL);
        if (c->conn) zfree(c->conn);
        zfree(c);
    }

    bool clientIsThrottled(client *c) {
        bool throttled = c->flag.throttled == 1;
        if (throttled) {
            EXPECT_EQ(c->conn->read_handler, nullptr);
            EXPECT_NE(c->throttler, nullptr);
            EXPECT_NE(c->throttle_node, nullptr);
            EXPECT_EQ(c->flag.throttle_checked, 1ULL);
            EXPECT_NE(c->throttle_start_us, 0ULL);
        } else {
            EXPECT_EQ(c->throttler, nullptr);
            EXPECT_EQ(c->throttle_node, nullptr);
            EXPECT_EQ(c->throttle_start_us, 0ULL);
        }
        return throttled;
    }

    void verifyThrottler(const char *metric_name, int clients_throttled, int cmds_throttled) {
        const throttleMetrics *m = throttle_getMetrics(metric_name);
        EXPECT_EQ(m->num_clients_throttled, clients_throttled);
        EXPECT_EQ(m->num_throttled_commands, cmds_throttled);
    }
};

using ThrottleDeathTest = ThrottleTest;

TEST_F(ThrottleTest, noThrottlerPassesThrough) {
    client *c = createFakeClient(1, true);
    /* No throttler registered yet, nothing to throttle. */
    EXPECT_FALSE(throttleClientIfNeeded(c));
    EXPECT_FALSE(clientIsThrottled(c));
    freeFakeClient(c);
}

TEST_F(ThrottleTest, throttleHappyCase) {
    int id = throttle_register(fakeWriteCriteria, NULL, "fake_throttler");
    client *c = createFakeClient(1, true);
    throttle_setRate(id, 0.0); // This will empty the bucket

    EXPECT_CALL(mock, aeCreateTimeEvent(_, _, _, _, _)).WillOnce(Return(1));
    EXPECT_TRUE(throttleClientIfNeeded(c));
    EXPECT_TRUE(clientIsThrottled(c));

    EXPECT_FALSE(throttleClientIfNeeded(c)); // We don't throttle client if it's already throttled
    verifyThrottler("fake_throttler", 1, 1);

    EXPECT_CALL(mock, aeDeleteTimeEvent(_, _)).WillOnce(Return(1));
    throttle_removeClient(c);
    EXPECT_FALSE(clientIsThrottled(c));
    verifyThrottler("fake_throttler", 0, 1);

    throttle_deregister(id);
    freeFakeClient(c);
}

TEST_F(ThrottleTest, criteriaMismatchPassesThrough) {
    int id = throttle_register(fakeWriteCriteria, NULL, "fake_throttler");
    client *c = createFakeClient(1, false); // client with read command

    EXPECT_FALSE(throttleClientIfNeeded(c));
    EXPECT_FALSE(clientIsThrottled(c));
    verifyThrottler("fake_throttler", 0, 0);

    throttle_deregister(id);
    freeFakeClient(c);
}

TEST_F(ThrottleTest, tokenAvailablePassesThrough) {
    int id = throttle_register(fakeWriteCriteria, NULL, "fake_throttler"); /* starts at UNLIMITED rate, full bucket */
    client *c = createFakeClient(1, true);

    /* Criteria matches, but tokens are available, consume token and proceed. */
    EXPECT_FALSE(throttleClientIfNeeded(c));
    EXPECT_FALSE(clientIsThrottled(c));
    verifyThrottler("fake_throttler", 0, 0);

    throttle_deregister(id);
    freeFakeClient(c);
}

TEST_F(ThrottleTest, deregisteredThrottlerDrainsButDoesNotThrottleNewClients) {
    int id = throttle_register(fakeWriteCriteria, NULL, "fake_throttler");
    throttle_setRate(id, 0.0);
    EXPECT_CALL(mock, aeCreateTimeEvent(_, _, _, _, _)).WillOnce(Return(1));

    /* Queue a client so the throttler cannot be freed on deregister. */
    client *queued = createFakeClient(1, true);
    EXPECT_TRUE(throttleClientIfNeeded(queued));
    EXPECT_TRUE(clientIsThrottled(queued));

    /* Deregister with a non-empty queue: the throttler stays alive in CLEANUP
     * state (still draining the queued client) but must not throttle new clients. */
    throttle_deregister(id);

    /* A new matching write command passes through untouched. */
    client *fresh = createFakeClient(2, true);
    EXPECT_FALSE(throttleClientIfNeeded(fresh));
    EXPECT_FALSE(clientIsThrottled(fresh));

    /* Drain the original client; emptying the queue frees the CLEANUP throttler. */
    EXPECT_CALL(mock, aeDeleteTimeEvent(_, _)).WillOnce(Return(1));
    throttle_removeClient(queued);

    freeFakeClient(queued);
    freeFakeClient(fresh);
}

TEST_F(ThrottleTest, strictestThrottlerThrottle) {
    /* Two throttlers both match a write command. The strictest (lowest rate)
     * wins: the client is queued under it and the multi-match flag is set so
     * the other bucket is charged on release. */
    int loose = throttle_register(fakeWriteCriteria, NULL, "loose"); /* UNLIMITED */
    int strict = throttle_register(fakeWriteCriteria, NULL, "strict");
    throttle_setRate(strict, 0.0); /* no tokens -> strictest */
    EXPECT_CALL(mock, aeCreateTimeEvent(_, _, _, _, _)).WillOnce(Return(1));

    client *c = createFakeClient(1, true);
    EXPECT_TRUE(throttleClientIfNeeded(c));
    EXPECT_TRUE(clientIsThrottled(c));
    EXPECT_EQ(c->flag.throttle_multi, 1ULL); /* matched >1 throttler */

    /* Queued under the strict throttler; the loose one is untouched. */
    verifyThrottler("strict", 1, 1);
    verifyThrottler("loose", 0, 0);

    EXPECT_CALL(mock, aeDeleteTimeEvent(_, _)).WillOnce(Return(1));
    throttle_removeClient(c);
    throttle_deregister(loose);
    throttle_deregister(strict);
    freeFakeClient(c);
}

TEST_F(ThrottleTest, strictestThrottlerNonThrottle) {
    /* Two throttlers both match, but the strictest still has a token, so the
     * client passes through (not throttled). Because it matched >1 throttler,
     * consumeOtherThrottlers also charges the other bucket on the pass path. */
    int loose = throttle_register(fakeWriteCriteria, NULL, "loose"); /* UNLIMITED */
    int strict = throttle_register(fakeWriteCriteria, NULL, "strict");
    throttle_setRate(strict, 1.0);

    client *c = createFakeClient(1, true);

    EXPECT_CALL(mock, tokenBucket_tryConsume(_, _, true)).WillOnce(Return(true)); // Force consume the loose bucket
    EXPECT_FALSE(throttleClientIfNeeded(c));                                      // token available -> passes through
    EXPECT_FALSE(clientIsThrottled(c));
    EXPECT_EQ(c->flag.throttle_multi, 0ULL); // multi flag is only set on the defer path */

    /* Neither throttler queued the client. */
    verifyThrottler("loose", 0, 0);
    verifyThrottler("strict", 0, 0);

    throttle_deregister(loose);
    throttle_deregister(strict);
    freeFakeClient(c);
}

TEST_F(ThrottleTest, adjustRatePolicyIncrease) {
    int id = throttle_register(fakeWriteCriteria, NULL, "fake_throttler"); /* starts UNLIMITED */

    /* Increase while already UNLIMITED is a no-op. */
    EXPECT_DOUBLE_EQ(throttle_adjustRate(id, 2.0), THROTTLE_UNLIMITED_RATE);

    /* Recover from halted state jumps to the fixed restart rate (100 ops/sec). */
    throttle_setRate(id, 0.0);
    EXPECT_DOUBLE_EQ(throttle_adjustRate(id, 2.0), 100.0);

    /* Normal increase: 100 * (2.0 - 1.0) = 200. */
    EXPECT_DOUBLE_EQ(throttle_adjustRate(id, 2.0), 200.0);

    /* Tiny multiplier still increases by the minimum step of 1 ops/sec. */
    EXPECT_DOUBLE_EQ(throttle_adjustRate(id, 1.00001), 201.0);

    throttle_deregister(id);
}

TEST_F(ThrottleTest, adjustRatePolicyDecrease) {
    int id = throttle_register(fakeWriteCriteria, NULL, "fake_throttler"); /* starts UNLIMITED */

    EXPECT_CALL(mock, tpsCalculator_averageTps(_)).WillRepeatedly(Return(500.0)); /* TPS floor */

    /* Normal decrease, well above the floor. */
    EXPECT_DOUBLE_EQ(throttle_adjustRate(id, 0.5), THROTTLE_UNLIMITED_RATE * 0.5);

    /* Decrease clamped up to the incoming-TPS floor: 1000 * 0.1 = 100 < 500. */
    throttle_setRate(id, 1000.0);
    EXPECT_DOUBLE_EQ(throttle_adjustRate(id, 0.1), 500.0);

    /* With no incoming TPS the floor is disabled: 500 * 0.1 = 50. */
    EXPECT_CALL(mock, tpsCalculator_averageTps(_)).WillRepeatedly(Return(0.0));
    EXPECT_DOUBLE_EQ(throttle_adjustRate(id, 0.1), 50.0);

    throttle_deregister(id);
}

TEST_F(ThrottleTest, setRate) {
    int id = throttle_register(fakeWriteCriteria, NULL, "fake_throttler");

    /* A normal rate is stored as-is. */
    throttle_setRate(id, 1234.0);
    EXPECT_DOUBLE_EQ(throttle_getMetrics("fake_throttler")->ops_per_sec, 1234.0);

    /* Above the unlimited ceiling is clamped down to THROTTLE_UNLIMITED_RATE. */
    throttle_setRate(id, THROTTLE_UNLIMITED_RATE * 2);
    EXPECT_DOUBLE_EQ(throttle_getMetrics("fake_throttler")->ops_per_sec, THROTTLE_UNLIMITED_RATE);

    /* Below epsilon collapses to zero. */
    throttle_setRate(id, 0.00001);
    EXPECT_DOUBLE_EQ(throttle_getMetrics("fake_throttler")->ops_per_sec, 0.0);

    throttle_deregister(id);
}

TEST_F(ThrottleTest, guardrailSecsTracking) {
    int id = throttle_register(fakeWriteCriteria, NULL, "fake_throttler");

    /* 0.05 ops/sec == 3 ops/min, at or below the 6 ops/min guardrail. */
    throttle_setRate(id, 0.05);
    fakeMonotimeUs += 3 * 1000000; /* advance 3 seconds */
    EXPECT_EQ(throttle_getGuardrailSecs(id), 3L);

    /* Back above the guardrail resets the timer. */
    throttle_setRate(id, 1.0); /* 60 ops/min */
    EXPECT_EQ(throttle_getGuardrailSecs(id), 0L);

    throttle_deregister(id);
}

// /* ---- A-layer: metrics aggregation ---- */

TEST_F(ThrottleTest, metricsAggregateAcrossSharedName) {
    /* Two throttlers sharing one metrics group ("shared") aggregate their metrics */
    int a = throttle_register(fakeWriteCriteria, NULL, "shared");
    int b = throttle_register(fakeWriteCriteria, NULL, "shared");

    /* ops_per_sec is the SUM of both throttlers' rates. */
    throttle_setRate(a, 100.0);
    throttle_setRate(b, 250.0);
    EXPECT_DOUBLE_EQ(throttle_getMetrics("shared")->ops_per_sec, 350.0);

    /* Empty both buckets. Both clients are writes matching both throttlers, so the
     * strictest (a, rate 0) wins and both queue under a; b stays empty. */
    throttle_setRate(a, 0.0);
    throttle_setRate(b, 0.0);
    EXPECT_CALL(mock, aeCreateTimeEvent(_, _, _, _, _)).WillOnce(Return(1));

    client *c1 = createFakeClient(1, true);
    EXPECT_TRUE(throttleClientIfNeeded(c1)); /* throttle_start_us = 100 (fake clock) */
    fakeMonotimeUs += 5 * 1000000;           /* +5s */
    client *c2 = createFakeClient(2, true);
    EXPECT_TRUE(throttleClientIfNeeded(c2)); /* throttle_start_us = 5,000,100 */

    const throttleMetrics *m = throttle_getMetrics("shared");
    /* Both clients increment the shared metrics group. */
    EXPECT_EQ(m->num_clients_throttled, 2);
    EXPECT_EQ(m->num_throttled_commands, 2);
    /* oldest_client_delay_us tracks the oldest queued client (c1, queued 5s ago). */
    EXPECT_EQ(m->oldest_client_delay_us, 5 * 1000000);

    EXPECT_CALL(mock, aeDeleteTimeEvent(_, _)).WillOnce(Return(1));
    throttle_removeClient(c1);
    throttle_removeClient(c2);
    throttle_deregister(a);
    throttle_deregister(b);
    freeFakeClient(c1);
    freeFakeClient(c2);
}

/* ---- Death tests ---- */

TEST_F(ThrottleDeathTest, deregisterThrottlerFail) {
    /* deregister a non-existent throttler */
    EXPECT_DEATH(throttle_deregister(99999), "");
}

TEST_F(ThrottleDeathTest, setRateNegativeAsserts) {
    int id = throttle_register(fakeWriteCriteria, NULL, "neg_rate");
    EXPECT_DEATH(throttle_setRate(id, -1.0), "");
    throttle_deregister(id);
}

TEST_F(ThrottleDeathTest, adjustRateOutOfRangeAsserts) {
    int id = throttle_register(fakeWriteCriteria, NULL, "bad_mult");
    EXPECT_DEATH(throttle_adjustRate(id, 3.5), ""); /* multiplier must be <= 3.0 */
    throttle_deregister(id);
}
