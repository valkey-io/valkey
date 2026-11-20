/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <csignal>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include "server.h"
}

/* The unit binary never runs initServer(), like valkey-check-rdb and
 * valkey-check-aof: there is no stack trace pipe, no THREADS_SIGNAL handler
 * and no server state. A panic must still finish its crash report and abort,
 * whatever stdin is. */
static void panicWithStdin(int idle_pipe) {
    int fd;
    int fds[2];

    server.crashlog_enabled = 1;
    server.logfile = (char *)"/dev/stderr"; /* So EXPECT_EXIT can match the report. */
    if (idle_pipe) {
        /* Keep the write end open, so a read from stdin blocks. */
        if (pipe(fds) == -1) _exit(2);
        fd = fds[0];
    } else {
        fd = open("/dev/null", O_RDONLY);
        if (fd == -1) _exit(2);
    }
    if (dup2(fd, STDIN_FILENO) == -1) _exit(2);
    /* A crash report that hangs dies of SIGALRM instead of blocking the suite. */
    alarm(10);
    serverPanic("crash report test");
}

class CrashReportDeathTest : public ::testing::Test {
  protected:
    void SetUp() override {
        /* Run the panic in a fresh process, without state from other tests. */
        GTEST_FLAG_SET(death_test_style, "threadsafe");
    }
};

TEST_F(CrashReportDeathTest, PanicWithIdleStdinFinishesReport) {
    EXPECT_EXIT(panicWithStdin(1), ::testing::KilledBySignal(SIGABRT), "BUG REPORT END");
}

TEST_F(CrashReportDeathTest, PanicWithEmptyStdinFinishesReport) {
    EXPECT_EXIT(panicWithStdin(0), ::testing::KilledBySignal(SIGABRT), "BUG REPORT END");
}
