/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

TEST(Maxmemory, FailedResolutionRetainsAppliedLimitAndRetries) {
    MockValkey mock;
    unsigned long long saved_limit = server.cgroup_memory_limit;
    unsigned long long saved_memory = server.system_memory_size;
    unsigned long long saved_maxmemory = server.maxmemory;
    int saved_percent = server.maxmemory_percent;
    int saved_error = server.cgroup_memory_error;
    int saved_verbosity = server.verbosity;
    char *saved_logfile = server.logfile;
    char logfile[] = "";

    server.cgroup_memory_limit = 200;
    server.system_memory_size = 1000;
    server.maxmemory = 1;
    server.maxmemory_percent = 1;
    server.logfile = logfile;
    server.verbosity = LL_WARNING + 1;

    EXPECT_CALL(mock, cgroupGetMemoryLimit(testing::_))
        .Times(2)
        .WillRepeatedly(testing::DoAll(testing::SetArgPointee<0>(0ULL), testing::Return(0)));
    for (int i = 0; i < 2; i++) {
        refreshMaxmemory();
        EXPECT_EQ(server.cgroup_memory_limit, 200ULL);
        EXPECT_EQ(server.maxmemory, 1ULL);
    }
    testing::Mock::VerifyAndClearExpectations(&mock);

    /* A readable limit whose percentage rounds to zero must also be retried. */
    EXPECT_CALL(mock, cgroupGetMemoryLimit(testing::_))
        .WillOnce(testing::DoAll(testing::SetArgPointee<0>(1ULL), testing::Return(0)))
        .WillOnce(testing::DoAll(testing::SetArgPointee<0>(1ULL), testing::Return(0)))
        .WillOnce(testing::DoAll(testing::SetArgPointee<0>(100ULL), testing::Return(0)));
    refreshMaxmemory();
    EXPECT_EQ(server.cgroup_memory_limit, 200ULL);
    EXPECT_EQ(server.maxmemory, 1ULL);
    server.maxmemory_percent = 100;
    refreshMaxmemory();
    EXPECT_EQ(server.cgroup_memory_limit, 1ULL);
    EXPECT_EQ(server.maxmemory, 1ULL);
    server.maxmemory_percent = 1;
    refreshMaxmemory();
    EXPECT_EQ(server.cgroup_memory_limit, 100ULL);
    EXPECT_EQ(server.maxmemory, 1ULL);

    server.cgroup_memory_limit = saved_limit;
    server.system_memory_size = saved_memory;
    server.maxmemory = saved_maxmemory;
    server.maxmemory_percent = saved_percent;
    server.cgroup_memory_error = saved_error;
    server.verbosity = saved_verbosity;
    server.logfile = saved_logfile;
}
