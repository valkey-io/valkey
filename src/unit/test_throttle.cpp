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
        server.logfile = (char *)"";
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

    /* A set_read_handler that always fails, to simulate a connection error. */
    static int failSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
        UNUSED(conn);
        UNUSED(func);
        return C_ERR;
    }

    client *createFakeClient(int client_id, bool write_command) {
        client *c = (client *)zcalloc(sizeof(client));
        c->id = client_id;
        c->conn = (connection *)zcalloc(sizeof(connection));
        c->conn->type = &dummyConnType;
        c->conn->read_handler = (ConnectionCallbackFunc)1;
        c->flag.pending_command = 1;
        c->argc = 1;
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
            EXPECT_NE(c->throttle_start, 0ULL);
        } else {
            EXPECT_EQ(c->throttler, nullptr);
            EXPECT_EQ(c->throttle_node, nullptr);
            EXPECT_EQ(c->throttle_start, 0ULL);
            EXPECT_EQ(c->flag.throttle_multi, 0ULL);
        }
        return throttled;
    }

    void verifyThrottler(const char *metric_name, int clients_throttled, int cmds_throttled) {
        throttleMetrics m;
        throttle_getMetrics(metric_name, &m);
        EXPECT_EQ(m.num_clients_throttled, clients_throttled);
        EXPECT_EQ(m.num_commands_throttled, cmds_throttled);
    }
};

using ThrottleDeathTest = ThrottleTest;

/* ---- throttle_throttleClientIfNeeded tests ---- */

TEST_F(ThrottleTest, noThrottlerPassesThrough) {
    client *c = createFakeClient(1, true);
    /* No throttler registered yet, nothing to throttle. */
    EXPECT_FALSE(throttle_throttleClientIfNeeded(c));
    EXPECT_FALSE(clientIsThrottled(c));
    freeFakeClient(c);
}

TEST_F(ThrottleTest, throttleHappyCase) {
    throttler *t = throttle_register(fakeWriteCriteria, NULL, "fake_throttler");
    client *c = createFakeClient(1, true);
    throttle_setRate(t, 0.0); // This will empty the bucket

    aeTimeProc *timeProc = NULL;
    void *clientData = NULL;
    EXPECT_CALL(mock, aeCreateTimeEvent(_, _, _, _, _))
        .WillOnce(DoAll(SaveArg<2>(&timeProc), SaveArg<3>(&clientData), Return(1)));
    EXPECT_TRUE(throttle_throttleClientIfNeeded(c));
    EXPECT_TRUE(clientIsThrottled(c));

    EXPECT_FALSE(throttle_throttleClientIfNeeded(c)); // We don't throttle client if it's already throttled
    verifyThrottler("fake_throttler", 1, 1);

    /* Drain via timeProc */
    throttle_setRate(t, THROTTLE_UNLIMITED_RATE);
    fakeMonotimeUs += 1000000;
    EXPECT_CALL(mock, queueClientForReprocessing(c)).Times(1);
    long long ret = timeProc(server.el, 1, clientData);
    EXPECT_EQ(ret, AE_NOMORE);

    EXPECT_FALSE(clientIsThrottled(c));
    verifyThrottler("fake_throttler", 0, 1);

    throttle_deregister(t);
    freeFakeClient(c);
}

TEST_F(ThrottleTest, criteriaMismatchPassesThrough) {
    throttler *t = throttle_register(fakeWriteCriteria, NULL, "fake_throttler");
    throttle_setRate(t, 0.0);
    client *c = createFakeClient(1, false); // client with read command

    EXPECT_CALL(mock, aeCreateTimeEvent(_, _, _, _, _)).Times(0);
    EXPECT_FALSE(throttle_throttleClientIfNeeded(c));
    EXPECT_FALSE(clientIsThrottled(c));
    verifyThrottler("fake_throttler", 0, 0);

    throttle_deregister(t);
    freeFakeClient(c);
}

TEST_F(ThrottleTest, tokenAvailablePassesThrough) {
    throttler *t = throttle_register(fakeWriteCriteria, NULL, "fake_throttler"); /* starts at UNLIMITED rate, full bucket */
    client *c = createFakeClient(1, true);

    /* Criteria matches, tokens are available, consume token and proceed. */
    EXPECT_CALL(mock, aeCreateTimeEvent(_, _, _, _, _)).Times(0);
    EXPECT_FALSE(throttle_throttleClientIfNeeded(c));
    EXPECT_FALSE(clientIsThrottled(c));
    verifyThrottler("fake_throttler", 0, 0);

    throttle_deregister(t);
    freeFakeClient(c);
}

TEST_F(ThrottleTest, deregisteredThrottlerDrainsButDoesNotThrottleNewClients) {
    throttler *t = throttle_register(fakeWriteCriteria, NULL, "fake_throttler");
    throttle_setRate(t, 0.0);

    aeTimeProc *timeProc = NULL;
    void *clientData = NULL;
    EXPECT_CALL(mock, aeCreateTimeEvent(_, _, _, _, _))
        .WillOnce(DoAll(SaveArg<2>(&timeProc), SaveArg<3>(&clientData), Return(1)));

    /* Queue a client so the throttler cannot be freed on deregister. */
    client *queued = createFakeClient(1, true);
    EXPECT_TRUE(throttle_throttleClientIfNeeded(queued));
    EXPECT_TRUE(clientIsThrottled(queued));

    /* Deregister with a non-empty queue: the throttler stays alive in CLEANUP
     * state (still draining the queued client) but must not throttle new clients. */
    throttle_deregister(t);

    /* A new matching write command passes through untouched. */
    client *fresh = createFakeClient(2, true);
    EXPECT_FALSE(throttle_throttleClientIfNeeded(fresh));
    EXPECT_FALSE(clientIsThrottled(fresh));
    verifyThrottler("fake_throttler", 1, 1);

    /* Drain via timeProc — deregister already set rate to UNLIMITED. */
    fakeMonotimeUs += 1000000;
    EXPECT_CALL(mock, queueClientForReprocessing(queued)).Times(1);
    long long ret = timeProc(server.el, 1, clientData);
    EXPECT_EQ(ret, AE_NOMORE);
    verifyThrottler("fake_throttler", 0, 1);

    freeFakeClient(queued);
    freeFakeClient(fresh);
}

TEST_F(ThrottleTest, strictestThrottlerThrottle) {
    /* Two throttlers both match a write command. The strictest (lowest rate)
     * wins: the client is queued under it and the multi-match flag is set so
     * the other bucket is charged on release. */
    throttler *loose = throttle_register(fakeWriteCriteria, NULL, "loose"); /* UNLIMITED */
    throttler *strict = throttle_register(fakeWriteCriteria, NULL, "strict");
    throttle_setRate(strict, 0.0); /* no tokens -> strictest */

    aeTimeProc *timeProc = NULL;
    void *clientData = NULL;
    EXPECT_CALL(mock, aeCreateTimeEvent(_, _, _, _, _))
        .WillOnce(DoAll(SaveArg<2>(&timeProc), SaveArg<3>(&clientData), Return(1)));

    client *c = createFakeClient(1, true);
    EXPECT_TRUE(throttle_throttleClientIfNeeded(c));
    EXPECT_TRUE(clientIsThrottled(c));
    EXPECT_EQ(c->flag.throttle_multi, 1ULL); /* matched >1 throttler */

    /* Queued under the strict throttler; the loose one is untouched. */
    verifyThrottler("strict", 1, 1);
    verifyThrottler("loose", 0, 0);

    /* Drain via timeProc. */
    throttle_setRate(strict, THROTTLE_UNLIMITED_RATE);
    fakeMonotimeUs += 1000000;
    EXPECT_CALL(mock, tokenBucket_tryConsume(_, _, false)).WillOnce(Return(true));
    EXPECT_CALL(mock, tokenBucket_tryConsume(_, _, true)).WillOnce(Return(true));
    EXPECT_CALL(mock, queueClientForReprocessing(c)).Times(1);
    long long ret = timeProc(server.el, 1, clientData);
    EXPECT_EQ(ret, AE_NOMORE);
    EXPECT_EQ(c->flag.throttle_multi, 0ULL);

    throttle_deregister(loose);
    throttle_deregister(strict);
    freeFakeClient(c);
}

TEST_F(ThrottleTest, strictestThrottlerNonThrottle) {
    /* Two throttlers both match, but the strictest still has a token, so the
     * client passes through (not throttled). Because it matched >1 throttler,
     * consumeOtherThrottlers also charges the other bucket on the pass path. */
    throttler *loose = throttle_register(fakeWriteCriteria, NULL, "loose"); /* UNLIMITED */
    throttler *strict = throttle_register(fakeWriteCriteria, NULL, "strict");
    throttle_setRate(strict, 1.0);

    client *c = createFakeClient(1, true);

    EXPECT_CALL(mock, tokenBucket_tryConsume(_, _, true)).WillOnce(Return(true)); // Force consume the loose bucket
    EXPECT_FALSE(throttle_throttleClientIfNeeded(c));                             // token available -> passes through
    EXPECT_FALSE(clientIsThrottled(c));
    EXPECT_EQ(c->flag.throttle_multi, 0ULL); // multi flag is only set on the defer path */

    /* Neither throttler queued the client. */
    verifyThrottler("loose", 0, 0);
    verifyThrottler("strict", 0, 0);

    throttle_deregister(loose);
    throttle_deregister(strict);
    freeFakeClient(c);
}

/* ---- throttler rate tests ---- */

TEST_F(ThrottleTest, adjustRatePolicyIncrease) {
    throttler *t = throttle_register(fakeWriteCriteria, NULL, "fake_throttler"); /* starts UNLIMITED */

    /* Increase while already UNLIMITED is a no-op. */
    EXPECT_DOUBLE_EQ(throttle_adjustRate(t, 2.0), THROTTLE_UNLIMITED_RATE);

    /* Recover from halted state jumps to the fixed restart rate (100 ops/sec). */
    throttle_setRate(t, 0.0);
    EXPECT_DOUBLE_EQ(throttle_adjustRate(t, 2.0), 100.0);

    /* Normal increase: 100 * (2.0 - 1.0) = 200. */
    EXPECT_DOUBLE_EQ(throttle_adjustRate(t, 2.0), 200.0);

    /* Tiny multiplier still increases by the minimum step of 1 ops/sec. */
    EXPECT_DOUBLE_EQ(throttle_adjustRate(t, 1.00001), 201.0);

    throttle_deregister(t);
}

TEST_F(ThrottleTest, adjustRatePolicyDecrease) {
    throttler *t = throttle_register(fakeWriteCriteria, NULL, "fake_throttler"); /* starts UNLIMITED */

    EXPECT_CALL(mock, tpsCalculator_averageTps(_)).WillRepeatedly(Return(500.0)); /* incoming TPS */

    /* Decreasing a rate that is still above incoming snaps straight down to incoming. */
    EXPECT_DOUBLE_EQ(throttle_adjustRate(t, 0.95), 500.0);

    /* Once at/below incoming, a further decrease goes below it (real throttling):
     * 500 * 0.8 = 400 < 500. */
    EXPECT_DOUBLE_EQ(throttle_adjustRate(t, 0.8), 400.0);

    /* With no measured incoming TPS the snap is disabled: 400 * 0.5 = 200. */
    EXPECT_CALL(mock, tpsCalculator_averageTps(_)).WillRepeatedly(Return(0.0));
    EXPECT_DOUBLE_EQ(throttle_adjustRate(t, 0.5), 200.0);

    throttle_deregister(t);
}

TEST_F(ThrottleTest, setRate) {
    throttler *t = throttle_register(fakeWriteCriteria, NULL, "fake_throttler");
    throttleMetrics m;

    /* A normal rate is stored as-is. */
    throttle_setRate(t, 1234.0);
    throttle_getMetrics("fake_throttler", &m);
    EXPECT_DOUBLE_EQ(m.ops_per_sec, 1234.0);

    /* Above the unlimited ceiling is clamped down to THROTTLE_UNLIMITED_RATE. */
    throttle_setRate(t, THROTTLE_UNLIMITED_RATE * 2);
    throttle_getMetrics("fake_throttler", &m);
    EXPECT_DOUBLE_EQ(m.ops_per_sec, THROTTLE_UNLIMITED_RATE);

    /* Below epsilon collapses to zero. */
    throttle_setRate(t, 0.00001);
    throttle_getMetrics("fake_throttler", &m);
    EXPECT_DOUBLE_EQ(m.ops_per_sec, 0.0);

    throttle_deregister(t);
}

TEST_F(ThrottleTest, guardrailSecsTracking) {
    throttler *t = throttle_register(fakeWriteCriteria, NULL, "fake_throttler");

    /* 0.05 ops/sec, at or below the 0.1 ops/sec guardrail. */
    throttle_setRate(t, 0.05);
    fakeMonotimeUs += 3 * 1000000; /* advance 3 seconds */
    EXPECT_EQ(throttle_getGuardrailSecs(t), 3L);

    /* Back above the guardrail resets the timer. */
    throttle_setRate(t, 1.0); /* above the 0.1 ops/sec guardrail */
    EXPECT_EQ(throttle_getGuardrailSecs(t), 0L);

    throttle_deregister(t);
}

/* ---- metrics aggregation ---- */

TEST_F(ThrottleTest, metricsAggregateAcrossSharedName) {
    /* Two throttlers sharing one metrics group ("shared") aggregate their metrics */
    throttler *a = throttle_register(fakeWriteCriteria, NULL, "shared");
    throttler *b = throttle_register(fakeWriteCriteria, NULL, "shared");

    /* ops_per_sec is the SUM of both throttlers' rates. */
    throttle_setRate(a, 100.0);
    throttle_setRate(b, 250.0);
    throttleMetrics m;
    throttle_getMetrics("shared", &m);
    EXPECT_DOUBLE_EQ(m.ops_per_sec, 350.0);

    /* Empty both buckets. Both clients are writes matching both throttlers, so the
     * strictest (a, rate 0) wins and both queue under a; b stays empty. */
    throttle_setRate(a, 0.0);
    throttle_setRate(b, 0.0);

    aeTimeProc *timeProc = NULL;
    void *clientData = NULL;
    EXPECT_CALL(mock, aeCreateTimeEvent(_, _, _, _, _))
        .WillOnce(DoAll(SaveArg<2>(&timeProc), SaveArg<3>(&clientData), Return(1)));

    client *c1 = createFakeClient(1, true);
    EXPECT_TRUE(throttle_throttleClientIfNeeded(c1)); /* throttle_start = 100 (fake clock) */
    fakeMonotimeUs += 5 * 1000000;                    /* +5s */
    client *c2 = createFakeClient(2, true);
    EXPECT_TRUE(throttle_throttleClientIfNeeded(c2)); /* throttle_start = 5,000,100 */

    throttle_getMetrics("shared", &m);
    /* Both clients increment the shared metrics group. */
    EXPECT_EQ(m.num_clients_throttled, 2);
    EXPECT_EQ(m.num_commands_throttled, 2);
    /* oldest_client_delay_us tracks the oldest queued client (c1, queued 5s ago). */
    EXPECT_EQ(m.oldest_client_delay_us, 5 * 1000000);

    /* Drain via timeProc. */
    throttle_setRate(a, THROTTLE_UNLIMITED_RATE);
    throttle_setRate(b, THROTTLE_UNLIMITED_RATE);
    fakeMonotimeUs += 1000000;
    /* Both clients have throttle_multi set (matched both throttlers), so each
     * release will also consume the other throttler's bucket. */
    EXPECT_CALL(mock, tokenBucket_tryConsume(_, _, false)).Times(2).WillRepeatedly(Return(true));
    EXPECT_CALL(mock, tokenBucket_tryConsume(_, _, true)).Times(2).WillRepeatedly(Return(true));
    EXPECT_CALL(mock, queueClientForReprocessing(_)).Times(2);
    long long ret = timeProc(server.el, 1, clientData);
    EXPECT_EQ(ret, AE_NOMORE);

    throttle_getMetrics("shared", &m);
    verifyThrottler("shared", 0, 2);
    EXPECT_EQ(m.oldest_client_delay_us, 0);

    throttle_deregister(a);
    throttle_deregister(b);
    freeFakeClient(c1);
    freeFakeClient(c2);
}

TEST_F(ThrottleTest, guardrailInfoReportsLongestPerType) {
    /* Two throttlers share a metrics name, both below guardrail. INFO reports a single line
     * per type, showing the longest-below-guardrail (earliest start) one. */
    throttler *a = throttle_register(fakeWriteCriteria, NULL, "shared");
    throttler *b = throttle_register(fakeWriteCriteria, NULL, "shared");

    throttle_setRate(a, 0.05);
    fakeMonotimeUs += 2000000;
    throttle_setRate(b, 0.05);
    fakeMonotimeUs += 3000000;

    sds info = throttle_sdscatInfoMetrics(sdsempty());

    /* Only throttler a guardrail secs is reported. */
    EXPECT_NE(strstr(info, "throttle_shared_guardrail_secs:5\r\n"), nullptr);
    EXPECT_EQ(strstr(info, "throttle_shared_guardrail_secs:3\r\n"), nullptr);

    sdsfree(info);
    throttle_deregister(a);
    throttle_deregister(b);
}

/* ---- throttlerTimeProc tests ---- */

TEST_F(ThrottleTest, timeProcHappyCaseOneCall) {
    /* When enough tokens are available, timeProc releases all queued clients in one call. */
    throttler *t = throttle_register(fakeWriteCriteria, NULL, "fake_throttler");
    throttle_setRate(t, 0.0);

    aeTimeProc *timeProc = NULL;
    void *clientData = NULL;
    EXPECT_CALL(mock, aeCreateTimeEvent(_, _, _, _, _))
        .WillOnce(DoAll(SaveArg<2>(&timeProc), SaveArg<3>(&clientData), Return(1)));

    client *c1 = createFakeClient(1, true);
    client *c2 = createFakeClient(2, true);
    EXPECT_TRUE(throttle_throttleClientIfNeeded(c1));
    EXPECT_TRUE(throttle_throttleClientIfNeeded(c2));

    /* Set unlimited rate so both clients are released in one timeProc call. */
    throttle_setRate(t, THROTTLE_UNLIMITED_RATE);
    fakeMonotimeUs += 1000000;

    EXPECT_CALL(mock, queueClientForReprocessing(_)).Times(2);

    long long ret = timeProc(server.el, 1, clientData);
    EXPECT_EQ(ret, AE_NOMORE);

    EXPECT_FALSE(clientIsThrottled(c1));
    EXPECT_FALSE(clientIsThrottled(c2));
    EXPECT_NE(c1->conn->read_handler, nullptr);
    EXPECT_NE(c2->conn->read_handler, nullptr);
    verifyThrottler("fake_throttler", 0, 2);

    throttle_deregister(t);
    freeFakeClient(c1);
    freeFakeClient(c2);
}

TEST_F(ThrottleTest, timeProcHappyCaseMultipleCall) {
    /* When the timer fires but tokens run out before the queue is empty, it reschedules. */
    throttler *t = throttle_register(fakeWriteCriteria, NULL, "fake_throttler");
    throttle_setRate(t, 0.0);

    aeTimeProc *timeProc = NULL;
    void *clientData = NULL;
    EXPECT_CALL(mock, aeCreateTimeEvent(_, _, _, _, _))
        .WillOnce(DoAll(SaveArg<2>(&timeProc), SaveArg<3>(&clientData), Return(1)));

    client *c1 = createFakeClient(1, true);
    client *c2 = createFakeClient(2, true);
    EXPECT_TRUE(throttle_throttleClientIfNeeded(c1));
    EXPECT_TRUE(throttle_throttleClientIfNeeded(c2));

    /* Set 1 ops/sec rate, so only 1 token available after refill. */
    throttle_setRate(t, 1.0);
    fakeMonotimeUs += 1000000;

    /* Only the first client will be released. No aeDeleteTimeEvent since queue stays non-empty. */
    EXPECT_CALL(mock, queueClientForReprocessing(c1)).Times(1);

    long long ret = timeProc(server.el, 1, clientData);
    /* Should return a positive wait time (reschedule). */
    EXPECT_EQ(ret, 100);

    /* c1 released, c2 still throttled. */
    EXPECT_FALSE(clientIsThrottled(c1));
    EXPECT_NE(c1->conn->read_handler, nullptr);
    EXPECT_TRUE(clientIsThrottled(c2));
    verifyThrottler("fake_throttler", 1, 2);

    /* Drain c2 for cleanup. */
    fakeMonotimeUs += 1000000; /* advance 1s, 1 token available */
    EXPECT_CALL(mock, queueClientForReprocessing(c2)).Times(1);
    ret = timeProc(server.el, 1, clientData);
    EXPECT_EQ(ret, AE_NOMORE);
    EXPECT_FALSE(clientIsThrottled(c2));
    EXPECT_NE(c2->conn->read_handler, nullptr);
    verifyThrottler("fake_throttler", 0, 2);

    throttle_deregister(t);
    freeFakeClient(c1);
    freeFakeClient(c2);
}

TEST_F(ThrottleTest, timeProcCallsFreeClientOnConnSetReadHandlerFailure) {
    /* If connSetReadHandler returns C_ERR, the client is freed. */
    throttler *t = throttle_register(fakeWriteCriteria, NULL, "fake_throttler");
    throttle_setRate(t, 0.0);

    aeTimeProc *timeProc = NULL;
    void *clientData = NULL;
    EXPECT_CALL(mock, aeCreateTimeEvent(_, _, _, _, _))
        .WillOnce(DoAll(SaveArg<2>(&timeProc), SaveArg<3>(&clientData), Return(1)));

    client *c = createFakeClient(1, true);
    EXPECT_TRUE(throttle_throttleClientIfNeeded(c));
    EXPECT_TRUE(clientIsThrottled(c));

    /* Install a failing read handler to simulate connection error. */
    static ConnectionType failConnType = {0};
    failConnType.set_read_handler = failSetReadHandler;
    c->conn->type = &failConnType;

    throttle_setRate(t, THROTTLE_UNLIMITED_RATE);
    fakeMonotimeUs += 1000000;

    /* freeClient should be called because connSetReadHandler fails. */
    EXPECT_CALL(mock, freeClient(c)).WillOnce(Return(0));
    EXPECT_CALL(mock, queueClientForReprocessing(_)).Times(0);

    long long ret = timeProc(server.el, 1, clientData);
    EXPECT_EQ(ret, AE_NOMORE);
    EXPECT_FALSE(clientIsThrottled(c));
    verifyThrottler("fake_throttler", 0, 1);

    throttle_deregister(t);
    freeFakeClient(c); // We still need to call it since freeClient is mocked.
}

TEST_F(ThrottleTest, timeProcMultiThrottlerConsumesOtherBuckets) {
    /* When a client matched multiple throttlers (throttle_multi flag), releasing it
     * via the timer should also consume tokens from the other throttlers. */
    throttler *loose = throttle_register(fakeWriteCriteria, NULL, "share");
    throttler *strict = throttle_register(fakeWriteCriteria, NULL, "share");
    throttle_setRate(strict, 0.0); /* strict has no tokens and client queues here */

    aeTimeProc *timeProc = NULL;
    void *clientData = NULL;
    EXPECT_CALL(mock, aeCreateTimeEvent(_, _, _, _, _))
        .WillOnce(DoAll(SaveArg<2>(&timeProc), SaveArg<3>(&clientData), Return(1)));

    client *c = createFakeClient(1, true);
    EXPECT_TRUE(throttle_throttleClientIfNeeded(c));
    EXPECT_EQ(c->flag.throttle_multi, 1ULL);

    /* Now release: set strict to high rate. */
    throttle_setRate(strict, THROTTLE_UNLIMITED_RATE);
    fakeMonotimeUs += 1000000;

    /* The strict throttler's own bucket is consumed (force_consume=false) in the while loop,
     * then consumeOtherThrottlers charges the loose throttler (force_consume=true). */
    EXPECT_CALL(mock, tokenBucket_tryConsume(_, _, false)).WillOnce(Return(true));
    EXPECT_CALL(mock, tokenBucket_tryConsume(_, _, true)).WillOnce(Return(true));
    EXPECT_CALL(mock, queueClientForReprocessing(c)).Times(1);

    long long ret = timeProc(server.el, 1, clientData);
    EXPECT_EQ(ret, AE_NOMORE);
    EXPECT_FALSE(clientIsThrottled(c));
    verifyThrottler("share", 0, 1);

    throttle_deregister(loose);
    throttle_deregister(strict);
    freeFakeClient(c);
}

TEST_F(ThrottleTest, timeProcCleanupThrottlerFreesOnDrain) {
    /* A deregistered throttler (CLEANUP state) is freed when the timer drains it. */
    throttler *t = throttle_register(fakeWriteCriteria, NULL, "fake_throttler");
    throttle_setRate(t, 0.0);

    aeTimeProc *timeProc = NULL;
    void *clientData = NULL;
    EXPECT_CALL(mock, aeCreateTimeEvent(_, _, _, _, _))
        .WillOnce(DoAll(SaveArg<2>(&timeProc), SaveArg<3>(&clientData), Return(1)));

    client *c = createFakeClient(1, true);
    EXPECT_TRUE(throttle_throttleClientIfNeeded(c));

    /* Deregister while client is still queued, enters CLEANUP state.
     * deregister sets rate to THROTTLE_UNLIMITED_RATE internally. */
    throttle_deregister(t);

    fakeMonotimeUs += 1000000;
    EXPECT_CALL(mock, queueClientForReprocessing(c)).Times(1);

    long long ret = timeProc(server.el, 1, clientData);
    EXPECT_EQ(ret, AE_NOMORE);
    EXPECT_FALSE(clientIsThrottled(c));
    verifyThrottler("fake_throttler", 0, 1);

    freeFakeClient(c);
}

/* ---- throttle_removeClient test ---- */
TEST_F(ThrottleTest, removeClient) {
    throttler *t = throttle_register(fakeWriteCriteria, NULL, "fake_throttler");
    throttle_setRate(t, 0.0);

    EXPECT_CALL(mock, aeCreateTimeEvent(_, _, _, _, _)).WillOnce(Return(1));

    client *c1 = createFakeClient(1, true);
    client *c2 = createFakeClient(2, true);
    EXPECT_TRUE(throttle_throttleClientIfNeeded(c1));
    EXPECT_TRUE(throttle_throttleClientIfNeeded(c2));
    EXPECT_TRUE(clientIsThrottled(c1));
    EXPECT_TRUE(clientIsThrottled(c2));
    verifyThrottler("fake_throttler", 2, 2);

    /* Remove the client from the throttler queue. */
    throttle_removeClient(c1);
    EXPECT_FALSE(clientIsThrottled(c1));
    EXPECT_EQ(c1->conn->read_handler, nullptr);
    verifyThrottler("fake_throttler", 1, 2);

    EXPECT_CALL(mock, aeDeleteTimeEvent(_, _)).WillOnce(Return(AE_OK));
    throttle_removeClient(c2);
    EXPECT_FALSE(clientIsThrottled(c2));
    EXPECT_EQ(c2->conn->read_handler, nullptr);
    verifyThrottler("fake_throttler", 0, 2);

    throttle_deregister(t);
    freeFakeClient(c1);
    freeFakeClient(c2);
}

/* ---- Death tests ---- */

TEST_F(ThrottleDeathTest, deregisterThrottlerFail) {
    /* deregister a NULL throttler */
    EXPECT_DEATH(throttle_deregister(NULL), "");
}

TEST_F(ThrottleDeathTest, setRateNegativeAsserts) {
    throttler *t = throttle_register(fakeWriteCriteria, NULL, "neg_rate");
    EXPECT_DEATH(throttle_setRate(t, -1.0), "");
    throttle_deregister(t);
}

TEST_F(ThrottleDeathTest, adjustRateOutOfRangeAsserts) {
    throttler *t = throttle_register(fakeWriteCriteria, NULL, "bad_mult");
    EXPECT_DEATH(throttle_adjustRate(t, 3.5), ""); /* multiplier must be <= 3.0 */
    throttle_deregister(t);
}
