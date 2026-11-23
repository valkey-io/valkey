/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "bio.h"
#include "functions.h"
#include "server.h"
#include "zmalloc.h"

int openNewIncrAofForAppend(void);
int rewriteAppendOnlyFileRio(rio *aof);
}

/* A buffer rio whose write fails on the Nth SELECT command header. */
static const char select_header[] = "*2\r\n$6\r\nSELECT\r\n";
static size_t (*buffer_write)(rio *, const void *, size_t);
static int select_writes;
static int fail_on_select;

static size_t failOnSelectWrite(rio *r, const void *buf, size_t len) {
    if (len == sizeof(select_header) - 1 && memcmp(buf, select_header, len) == 0 &&
        ++select_writes == fail_on_select) {
        return 0;
    }
    return buffer_write(r, buf, len);
}

static void addStringKey(serverDb *db, const char *key, const char *val) {
    sds keysds = sdsnew(key);
    robj *o = createStringObjectWithKeyAndExpire(val, strlen(val), keysds, -1);
    sdsfree(keysds);
    ASSERT_TRUE(kvstoreHashtableAdd(db->keys, 0, o));
}

/* Two created databases with only a keys kvstore. Unit tests run one per
 * process, so the global state set here is not restored. */
static void setupTwoDbs(void) {
    functionsInit();
    server.child_info_pipe[1] = -1;
    server.dbnum = 2;
    server.db = (serverDb **)zcalloc(sizeof(serverDb *) * server.dbnum);
    server.created_db_ids = (int *)zcalloc(sizeof(int) * server.dbnum);
    server.created_db_count = server.dbnum;
    server.created_db_capacity = server.dbnum;
    for (int j = 0; j < server.dbnum; j++) {
        server.db[j] = (serverDb *)zcalloc(sizeof(serverDb));
        server.db[j]->id = j;
        server.created_db_ids[j] = j;
        server.db[j]->keys = kvstoreCreate(&kvstoreKeysHashtableType, 0, 0);
    }
}

/* The SELECT for the second non-empty db fails after the iterator of the
 * first db was released. The error path must not release it again. */
TEST(AofRewriteTest, selectWriteErrorOnSecondDbReleasesIteratorOnce) {
    setupTwoDbs();
    addStringKey(server.db[0], "k0", "v0");
    addStringKey(server.db[1], "k1", "v1");

    size_t used_before = zmalloc_used_memory();
    rio r;
    rioInitWithBuffer(&r, sdsempty());
    buffer_write = r.write;
    r.write = failOnSelectWrite;
    select_writes = 0;
    fail_on_select = 2;

    EXPECT_EQ(rewriteAppendOnlyFileRio(&r), C_ERR);
    EXPECT_EQ(select_writes, 2);

    sdsfree(r.io.buffer.ptr);
    /* A second zfree of the db 0 iterator also subtracts its size twice. */
    EXPECT_EQ(zmalloc_used_memory(), used_before);
}

using ::testing::Return;

class AofFsyncOffsetTest : public ::testing::Test {
  protected:
    MockValkey mock;

    void SetUp() override {
        memset(&server, 0, sizeof(valkeyServer));
        server.verbosity = LL_NOTHING;
        server.aof_state = AOF_ON;
        server.aof_fsync = AOF_FSYNC_EVERYSEC;
        server.aof_buf = sdsempty();
        server.aof_fd = -1;
        server.aof_last_write_status = C_OK;
        server.aof_bio_fsync_status = C_OK;
        server.aof_last_incr_size = 200;
        server.aof_last_incr_fsync_offset = 200;
        server.aof_last_fsync = 1000;
        server.mstime = 1500;
        server.primary_repl_offset = 200;
        server.fsynced_reploff = 100;
        server.fsynced_reploff_pending = 100;
    }

    void TearDown() override {
        if (server.aof_buf != NULL) {
            sdsfree(server.aof_buf);
            server.aof_buf = NULL;
        }
    }
};

TEST_F(AofFsyncOffsetTest, DoesNotAdvanceAfterBioFsyncError) {
    server.aof_bio_fsync_status = C_ERR;
    server.aof_bio_fsync_errno = EIO;

    EXPECT_CALL(mock, bioPendingJobsOfType(BIO_AOF_FSYNC)).WillRepeatedly(Return(0UL));
    EXPECT_CALL(mock, bioPendingJobsOfType(BIO_CLOSE_AOF)).WillRepeatedly(Return(0UL));

    flushAppendOnlyFile(0);

    EXPECT_EQ(server.fsynced_reploff_pending, 100);
    EXPECT_EQ(server.fsynced_reploff, 100);
}

TEST_F(AofFsyncOffsetTest, DoesNotAdvanceWhileBioFsyncInProgress) {
    server.aof_bio_fsync_status = C_OK;

    EXPECT_CALL(mock, bioPendingJobsOfType(BIO_AOF_FSYNC)).WillRepeatedly(Return(1UL));
    EXPECT_CALL(mock, bioPendingJobsOfType(BIO_CLOSE_AOF)).WillRepeatedly(Return(0UL));

    flushAppendOnlyFile(0);

    EXPECT_EQ(server.fsynced_reploff_pending, 100);
}

TEST_F(AofFsyncOffsetTest, DoesNotAdvanceDuringRotationBioCloseAof) {
    server.aof_last_incr_size = 0;
    server.aof_last_incr_fsync_offset = 0;
    server.aof_bio_fsync_status = C_OK;

    EXPECT_CALL(mock, bioPendingJobsOfType(BIO_AOF_FSYNC)).WillRepeatedly(Return(0UL));
    EXPECT_CALL(mock, bioPendingJobsOfType(BIO_CLOSE_AOF)).WillRepeatedly(Return(1UL));

    flushAppendOnlyFile(0);

    EXPECT_EQ(server.fsynced_reploff_pending, 100);
}

TEST_F(AofFsyncOffsetTest, DoesNotAdvanceBeforeEverysecFsyncRequested) {
    server.aof_last_incr_fsync_offset = 100;
    server.aof_last_incr_size = 200;
    server.mstime = 1200;

    EXPECT_CALL(mock, bioPendingJobsOfType(BIO_AOF_FSYNC)).WillRepeatedly(Return(0UL));
    EXPECT_CALL(mock, bioPendingJobsOfType(BIO_CLOSE_AOF)).WillRepeatedly(Return(0UL));

    flushAppendOnlyFile(0);

    EXPECT_EQ(server.fsynced_reploff_pending, 100);
}

TEST_F(AofFsyncOffsetTest, DoesNotAdvanceAfterWriteError) {
    server.aof_last_write_status = C_ERR;

    EXPECT_CALL(mock, bioPendingJobsOfType(BIO_AOF_FSYNC)).WillRepeatedly(Return(0UL));
    EXPECT_CALL(mock, bioPendingJobsOfType(BIO_CLOSE_AOF)).WillRepeatedly(Return(0UL));

    flushAppendOnlyFile(0);

    EXPECT_EQ(server.fsynced_reploff_pending, 100);
}

TEST_F(AofFsyncOffsetTest, AdvancesForNoAofOffsetWhenHealthy) {
    server.aof_bio_fsync_status = C_OK;
    server.fsynced_reploff_pending = 200;
    server.primary_repl_offset = 250;

    EXPECT_CALL(mock, bioPendingJobsOfType(BIO_AOF_FSYNC)).WillRepeatedly(Return(0UL));
    EXPECT_CALL(mock, bioPendingJobsOfType(BIO_CLOSE_AOF)).WillRepeatedly(Return(0UL));

    flushAppendOnlyFile(0);
    EXPECT_EQ(server.fsynced_reploff_pending, 250);
}

TEST_F(AofFsyncOffsetTest, DoesNotAdvanceWhenAppendfsyncNo) {
    server.aof_fsync = AOF_FSYNC_NO;

    EXPECT_CALL(mock, bioPendingJobsOfType(BIO_AOF_FSYNC)).WillRepeatedly(Return(0UL));
    EXPECT_CALL(mock, bioPendingJobsOfType(BIO_CLOSE_AOF)).WillRepeatedly(Return(0UL));

    flushAppendOnlyFile(0);

    EXPECT_EQ(server.fsynced_reploff_pending, 100);
}

class AofBioRecoveryTest : public ::testing::Test {
  protected:
    char tmpdir[128];
    char orig_cwd[PATH_MAX];
    bool has_tmpdir = false;
    bool chdir_ok = false;

    static void SetUpTestSuite() {
        memset(&server, 0, sizeof(valkeyServer));
        server.verbosity = LL_NOTHING;
        bioInit();
    }

    static void TearDownTestSuite() {
        bioDrainWorker(BIO_AOF_FSYNC);
        bioDrainWorker(BIO_CLOSE_AOF);
        bioDrainWorker(BIO_CLOSE_FILE);
        bioKillThreads();
    }

    struct ScopedFd {
        int fd;
        explicit ScopedFd(int f) :
            fd(f) {
        }
        ~ScopedFd() {
            if (fd >= 0) close(fd);
        }
        int release() {
            int f = fd;
            fd = -1;
            return f;
        }
        ScopedFd(const ScopedFd &) = delete;
        ScopedFd &operator=(const ScopedFd &) = delete;
    };

    void SetUp() override {
        orig_cwd[0] = 0;
        ASSERT_NE(getcwd(orig_cwd, sizeof(orig_cwd)), nullptr);
        snprintf(tmpdir, sizeof(tmpdir), "/tmp/valkey_aof_unit_XXXXXX");
        ASSERT_NE(mkdtemp(tmpdir), nullptr);
        has_tmpdir = true;
        ASSERT_EQ(chdir(tmpdir), 0);
        chdir_ok = true;

        server.verbosity = LL_NOTHING;
        server.logfile = (char *)"";
        server.aof_state = AOF_ON;
        server.aof_fsync = AOF_FSYNC_EVERYSEC;
        server.aof_use_rdb_preamble = 1;
        server.aof_rewrite_use_rdb_preamble = 1;
        server.aof_dirname = sdsnew("appendonlydir");
        server.aof_filename = sdsnew("appendonly.aof");
        server.aof_buf = sdsempty();
        server.aof_fd = -1;
        server.aof_last_write_status = C_OK;
        server.aof_last_write_errno = 0;
        server.aof_bio_fsync_status = C_OK;
        server.aof_bio_fsync_errno = 0;
        server.aof_fd_gen = 0;
        server.aof_bio_fsync_error_gen = 0;
        server.aof_rewrite_base_fd_gen = 0;
        server.aof_last_incr_size = 0;
        server.aof_last_incr_fsync_offset = 0;
        server.aof_current_size = 0;
        server.aof_rewrite_base_size = 0;
        server.aof_lastbgrewrite_status = C_OK;
        server.stat_aofrw_consecutive_failures = 0;
        server.child_pid = -1;
        server.aof_last_fsync = 1000;
        server.mstime = 1500;
        server.primary_repl_offset = 200;
        server.fsynced_reploff = 100;
        server.fsynced_reploff_pending = 100;
    }

    void TearDown() override {
        bioDrainWorker(BIO_AOF_FSYNC);
        bioDrainWorker(BIO_CLOSE_AOF);
        bioDrainWorker(BIO_CLOSE_FILE);
        if (server.aof_fd != -1) {
            close(server.aof_fd);
            server.aof_fd = -1;
        }
        if (server.aof_manifest != NULL) {
            aofManifestFree(server.aof_manifest);
            server.aof_manifest = NULL;
        }
        if (server.aof_buf != NULL) {
            sdsfree(server.aof_buf);
            server.aof_buf = NULL;
        }
        if (server.aof_dirname != NULL) {
            sdsfree(server.aof_dirname);
            server.aof_dirname = NULL;
        }
        if (server.aof_filename != NULL) {
            sdsfree(server.aof_filename);
            server.aof_filename = NULL;
        }
        if (chdir_ok) {
            ASSERT_EQ(chdir(orig_cwd), 0);
        }
        if (has_tmpdir) {
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
            int rc = system(cmd);
            (void)rc;
        }
    }

    int createTempFile(const char *name) {
        int fd = open(name, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd != -1) {
            const char data[] = "*1\r\n$4\r\nPING\r\n";
            ssize_t n = write(fd, data, sizeof(data) - 1);
            (void)n;
        }
        return fd;
    }

    void initAofDirAndOpen() {
        ASSERT_EQ(mkdir("appendonlydir", 0755), 0);
        {
            ScopedFd bfd(createTempFile("appendonlydir/appendonly.aof.1.base.rdb"));
            ASSERT_GE(bfd.fd, 0);
        }
        {
            ScopedFd ifd(createTempFile("appendonlydir/appendonly.aof.1.incr.aof"));
            ASSERT_GE(ifd.fd, 0);
        }
        {
            ScopedFd mfd(open("appendonlydir/appendonly.aof.manifest", O_WRONLY | O_CREAT | O_TRUNC, 0644));
            ASSERT_GE(mfd.fd, 0);
            const char manifest[] =
                "file appendonly.aof.1.base.rdb seq 1 type b\n"
                "file appendonly.aof.1.incr.aof seq 1 type i\n";
            ASSERT_EQ(write(mfd.fd, manifest, sizeof(manifest) - 1), (ssize_t)(sizeof(manifest) - 1));
        }
        aofLoadManifestFromDisk();
        aofOpenIfNeededOnServerStart();
        ASSERT_GE(server.aof_fd, 0);
    }

    void createRewriteBaseTempFile(pid_t pid) {
        char tmpbase[128];
        snprintf(tmpbase, sizeof(tmpbase), "temp-rewriteaof-bg-%d.aof", (int)pid);
        ScopedFd bfd(createTempFile(tmpbase));
        ASSERT_GE(bfd.fd, 0);
        ASSERT_EQ(fsync(bfd.fd), 0);
    }
};

TEST_F(AofBioRecoveryTest, BioCloseAofDoesNotClearErrorOrAdvanceOffset) {
    int fd = createTempFile("old_failed.incr.aof");
    ASSERT_GE(fd, 0);

    server.aof_bio_fsync_status = C_ERR;
    server.aof_bio_fsync_errno = EIO;
    server.aof_last_incr_size = 0;
    server.aof_last_incr_fsync_offset = 0;
    server.fsynced_reploff = 100;
    server.fsynced_reploff_pending = 100;
    server.primary_repl_offset = 200;

    bioCreateCloseAofJob(fd, 200, 0);
    bioDrainWorker(BIO_CLOSE_AOF);

    EXPECT_EQ(fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(server.aof_bio_fsync_status, C_ERR);
    EXPECT_EQ(server.fsynced_reploff_pending, 100);

    flushAppendOnlyFile(0);
    EXPECT_EQ(server.fsynced_reploff_pending, 100);
}

TEST_F(AofBioRecoveryTest, BioAofFsyncDoesNotClearErrorOrAdvanceOffset) {
    int fd = createTempFile("newer_incr.aof");
    ASSERT_GE(fd, 0);
    server.aof_fd = fd;

    server.aof_bio_fsync_status = C_ERR;
    server.aof_bio_fsync_errno = EIO;
    server.fsynced_reploff = 100;
    server.fsynced_reploff_pending = 100;
    server.primary_repl_offset = 250;

    bioCreateFsyncJob(fd, 250, 0);
    bioDrainWorker(BIO_AOF_FSYNC);

    EXPECT_EQ(server.aof_bio_fsync_status, C_ERR);
    EXPECT_EQ(server.fsynced_reploff_pending, 100);
}

TEST_F(AofBioRecoveryTest, FailedRewritePreservesErrorAndDurablePrefix) {
    initAofDirAndOpen();

    server.aof_bio_fsync_status = C_ERR;
    server.aof_bio_fsync_errno = EIO;
    server.aof_bio_fsync_error_gen = server.aof_fd_gen;
    server.fsynced_reploff = 100;
    server.fsynced_reploff_pending = 100;
    server.primary_repl_offset = 200;

    ASSERT_EQ(openNewIncrAofForAppend(), C_OK);
    bioDrainWorker(BIO_CLOSE_AOF);

    /* Child rewrite exits with failure; latched C_ERR and durable prefix must remain intact. */
    server.child_pid = 42424;
    backgroundRewriteDoneHandler(1, 0);

    EXPECT_EQ(server.aof_lastbgrewrite_status, C_ERR);
    EXPECT_EQ(server.aof_bio_fsync_status, C_ERR);
    EXPECT_EQ(server.fsynced_reploff_pending, 100);
    EXPECT_EQ(server.fsynced_reploff, 100);

    flushAppendOnlyFile(0);
    EXPECT_EQ(server.fsynced_reploff_pending, 100);
}

TEST_F(AofBioRecoveryTest, CoveringRewriteClearsErrorAndAdvancesWhenIncrEmpty) {
    initAofDirAndOpen();

    server.aof_bio_fsync_status = C_ERR;
    server.aof_bio_fsync_errno = EIO;
    server.aof_bio_fsync_error_gen = server.aof_fd_gen;
    server.fsynced_reploff = 100;
    server.fsynced_reploff_pending = 100;
    server.primary_repl_offset = 200;

    /* Rotate and drain BIO_CLOSE_AOF before rewrite commits: C_ERR must remain latched. */
    ASSERT_EQ(openNewIncrAofForAppend(), C_OK);
    bioDrainWorker(BIO_CLOSE_AOF);
    EXPECT_EQ(server.aof_bio_fsync_status, C_ERR);
    EXPECT_EQ(server.fsynced_reploff_pending, 100);

    /* Commit a real replacement base file and manifest. */
    server.child_pid = 42425;
    createRewriteBaseTempFile(server.child_pid);

    backgroundRewriteDoneHandler(0, 0);

    EXPECT_EQ(server.aof_lastbgrewrite_status, C_OK);
    EXPECT_EQ(server.aof_bio_fsync_status, C_OK);

    flushAppendOnlyFile(0);
    EXPECT_EQ(server.fsynced_reploff_pending, 200);
}

TEST_F(AofBioRecoveryTest, CoveringRewriteRequiresDeltaFsyncBeforeAdvancingNewIncr) {
    initAofDirAndOpen();

    server.aof_bio_fsync_status = C_ERR;
    server.aof_bio_fsync_errno = EIO;
    server.aof_bio_fsync_error_gen = server.aof_fd_gen;
    server.fsynced_reploff = 100;
    server.fsynced_reploff_pending = 100;
    server.primary_repl_offset = 200;

    ASSERT_EQ(openNewIncrAofForAppend(), C_OK);
    bioDrainWorker(BIO_CLOSE_AOF);
    EXPECT_EQ(server.aof_bio_fsync_status, C_ERR);
    EXPECT_EQ(server.fsynced_reploff_pending, 100);

    server.child_pid = 42426;
    createRewriteBaseTempFile(server.child_pid);

    /* Simulate writes appended to the new incremental file during rewrite whose
     * BIO_AOF_FSYNC skipped fsync while C_ERR was latched. */
    server.aof_last_incr_size = 64;
    server.aof_last_incr_fsync_offset = 64;
    server.primary_repl_offset = 260;

    backgroundRewriteDoneHandler(0, 0);

    EXPECT_EQ(server.aof_lastbgrewrite_status, C_OK);
    EXPECT_EQ(server.aof_bio_fsync_status, C_OK);
    EXPECT_EQ(server.aof_last_incr_fsync_offset, 0);

    /* Empty-buffer flush must NOT jump to 260 until the new incremental delta is fsynced. */
    flushAppendOnlyFile(0);
    EXPECT_EQ(server.fsynced_reploff_pending, 100);

    /* Once a real BIO_AOF_FSYNC on the new incremental fd succeeds, offset 260 is published. */
    bioCreateFsyncJob(server.aof_fd, 260, 0);
    bioDrainWorker(BIO_AOF_FSYNC);
    server.aof_last_incr_fsync_offset = server.aof_last_incr_size;
    EXPECT_EQ(server.fsynced_reploff_pending, 260);

    /* Subsequent NO_AOF / ping-only offset bump advances cleanly. */
    server.primary_repl_offset = 290;
    flushAppendOnlyFile(0);
    EXPECT_EQ(server.fsynced_reploff_pending, 290);
}

TEST_F(AofBioRecoveryTest, RewriteDoesNotClearErrorOnNewIncrGen) {
    initAofDirAndOpen();

    ASSERT_EQ(openNewIncrAofForAppend(), C_OK);
    bioDrainWorker(BIO_CLOSE_AOF);

    /* Simulate a BIO fsync error that occurred on the post-rotation incremental fd. */
    server.aof_bio_fsync_status = C_ERR;
    server.aof_bio_fsync_errno = EIO;
    server.aof_bio_fsync_error_gen = server.aof_rewrite_base_fd_gen;
    server.fsynced_reploff = 100;
    server.fsynced_reploff_pending = 100;
    server.primary_repl_offset = 260;

    server.child_pid = 42427;
    createRewriteBaseTempFile(server.child_pid);

    backgroundRewriteDoneHandler(0, 0);

    EXPECT_EQ(server.aof_lastbgrewrite_status, C_OK);
    EXPECT_EQ(server.aof_bio_fsync_status, C_ERR);
    EXPECT_EQ(server.aof_bio_fsync_error_gen, server.aof_rewrite_base_fd_gen);
    EXPECT_EQ(server.fsynced_reploff_pending, 100);
}

TEST_F(AofBioRecoveryTest, BackgroundRewriteDoneDrainsBioCloseAndHandlesWaitRewrite) {
    initAofDirAndOpen();

    server.aof_state = AOF_WAIT_REWRITE;
    server.aof_bio_fsync_status = C_ERR;
    server.aof_bio_fsync_errno = EIO;
    server.aof_bio_fsync_error_gen = server.aof_fd_gen;
    server.fsynced_reploff = 100;
    server.fsynced_reploff_pending = 100;
    server.primary_repl_offset = 200;

    /* Leave the rotation BIO_CLOSE_AOF job in-flight for backgroundRewriteDoneHandler to drain. */
    ASSERT_EQ(openNewIncrAofForAppend(), C_OK);

    server.child_pid = 42428;
    createRewriteBaseTempFile(server.child_pid);

    backgroundRewriteDoneHandler(0, 0);

    EXPECT_EQ(server.aof_lastbgrewrite_status, C_OK);
    EXPECT_EQ(server.aof_state, AOF_ON);
    EXPECT_EQ(server.aof_bio_fsync_status, C_OK);
    EXPECT_EQ(bioPendingJobsOfType(BIO_CLOSE_AOF), 0UL);
    EXPECT_EQ(bioPendingJobsOfType(BIO_AOF_FSYNC), 0UL);
}

TEST_F(AofBioRecoveryTest, AppendfsyncAlwaysDoesNotPublishAcrossPendingOrFailedBioClose) {
    initAofDirAndOpen();

    server.fsynced_reploff = 100;
    server.fsynced_reploff_pending = 100;
    server.primary_repl_offset = 200;

    /* Rotate to a new incremental segment as BGREWRITEAOF does, then drain the
     * real close job so we can deterministically control pending BIO_CLOSE_AOF
     * and subsequent EIO latch states on the new incremental descriptor. */
    ASSERT_EQ(openNewIncrAofForAppend(), C_OK);
    bioDrainWorker(BIO_CLOSE_AOF);

    server.fsynced_reploff = 100;
    server.fsynced_reploff_pending = 100;
    server.aof_fsync = AOF_FSYNC_ALWAYS;

    /* Phase 1: Old BIO_CLOSE_AOF is still pending (status still C_OK).
     * Synchronous AOF_FSYNC_ALWAYS flush on the new incremental segment must NOT
     * advance fsynced_reploff_pending across the unresolved old segment. */
    {
        MockValkey mock;
        EXPECT_CALL(mock, bioPendingJobsOfType(BIO_AOF_FSYNC)).WillRepeatedly(Return(0UL));
        EXPECT_CALL(mock, bioPendingJobsOfType(BIO_CLOSE_AOF)).WillRepeatedly(Return(1UL));

        server.aof_buf = sdscat(server.aof_buf, "*1\r\n$4\r\nPING\r\n");
        server.primary_repl_offset = 250;
        flushAppendOnlyFile(0);

        EXPECT_EQ(server.fsynced_reploff_pending, 100);
        EXPECT_EQ(server.fsynced_reploff, 100);
    }

    /* Phase 2: Old BIO_CLOSE_AOF returns EIO (latching C_ERR) while the
     * replacement base is uncommitted. Subsequent AOF_FSYNC_ALWAYS flushes on
     * the new incremental segment must keep fsynced_reploff_pending at 100. */
    server.aof_bio_fsync_status = C_ERR;
    server.aof_bio_fsync_errno = EIO;
    server.aof_buf = sdscat(server.aof_buf, "*1\r\n$4\r\nPING\r\n");
    server.primary_repl_offset = 300;
    flushAppendOnlyFile(0);
    EXPECT_EQ(server.fsynced_reploff_pending, 100);

    flushAppendOnlyFile(0);
    EXPECT_EQ(server.fsynced_reploff_pending, 100);
}

static std::atomic<int> g_eio_fsync_calls{0};

static int failAofFsyncWithEio(int) {
    g_eio_fsync_calls.fetch_add(1, std::memory_order_relaxed);
    errno = EIO;
    return -1;
}

TEST_F(AofBioRecoveryTest, BioAofFsyncEioLatchesGenerationAndSuppressesSubsequentFsyncs) {
    ScopedFd fd(createTempFile("eio_fsync.incr.aof"));
    ASSERT_GE(fd.fd, 0);
    ScopedFd close_fd(dup(fd.fd));
    ASSERT_GE(close_fd.fd, 0);

    server.aof_bio_fsync_status = C_OK;
    server.aof_bio_fsync_errno = 0;
    server.aof_bio_fsync_error_gen = 0;
    server.aof_fd_gen = 7;
    server.fsynced_reploff = 200;
    server.fsynced_reploff_pending = 200;
    g_eio_fsync_calls.store(0, std::memory_order_relaxed);

    {
        MockValkey mock;
        EXPECT_CALL(mock, aofFsync(::testing::_)).WillRepeatedly(::testing::Invoke(failAofFsyncWithEio));

        bioCreateFsyncJob(fd.fd, 500, 0);
        bioDrainWorker(BIO_AOF_FSYNC);

        EXPECT_EQ(server.aof_bio_fsync_status, C_ERR);
        EXPECT_EQ(server.aof_bio_fsync_errno, EIO);
        EXPECT_EQ(server.aof_bio_fsync_error_gen, 7ULL);
        EXPECT_EQ(server.fsynced_reploff_pending, 200);
        EXPECT_EQ(g_eio_fsync_calls.load(std::memory_order_relaxed), 1);

        bioCreateFsyncJob(fd.fd, 600, 0);
        bioCreateCloseAofJob(close_fd.release(), 650, 0);
        bioDrainWorker(BIO_AOF_FSYNC);
        bioDrainWorker(BIO_CLOSE_AOF);

        EXPECT_EQ(g_eio_fsync_calls.load(std::memory_order_relaxed), 1);
        EXPECT_EQ(server.aof_bio_fsync_status, C_ERR);
        EXPECT_EQ(server.aof_bio_fsync_error_gen, 7ULL);
        EXPECT_EQ(server.fsynced_reploff_pending, 200);
    }
}

TEST_F(AofBioRecoveryTest, BioCloseAofEioLatchesGenerationAndSuppressesSubsequentFsyncs) {
    ScopedFd close_fd(createTempFile("eio_close.incr.aof"));
    ASSERT_GE(close_fd.fd, 0);
    ScopedFd next_fd(createTempFile("eio_next.incr.aof"));
    ASSERT_GE(next_fd.fd, 0);

    server.aof_bio_fsync_status = C_OK;
    server.aof_bio_fsync_errno = 0;
    server.aof_bio_fsync_error_gen = 0;
    server.aof_fd_gen = 9;
    server.fsynced_reploff = 300;
    server.fsynced_reploff_pending = 300;
    g_eio_fsync_calls.store(0, std::memory_order_relaxed);

    {
        MockValkey mock;
        EXPECT_CALL(mock, aofFsync(::testing::_)).WillRepeatedly(::testing::Invoke(failAofFsyncWithEio));

        bioCreateCloseAofJob(close_fd.release(), 800, 0);
        bioDrainWorker(BIO_CLOSE_AOF);

        EXPECT_EQ(server.aof_bio_fsync_status, C_ERR);
        EXPECT_EQ(server.aof_bio_fsync_errno, EIO);
        EXPECT_EQ(server.aof_bio_fsync_error_gen, 9ULL);
        EXPECT_EQ(server.fsynced_reploff_pending, 300);
        EXPECT_EQ(g_eio_fsync_calls.load(std::memory_order_relaxed), 1);

        server.aof_fd_gen = 10;
        bioCreateFsyncJob(next_fd.fd, 900, 0);
        bioDrainWorker(BIO_AOF_FSYNC);

        EXPECT_EQ(g_eio_fsync_calls.load(std::memory_order_relaxed), 1);
        EXPECT_EQ(server.aof_bio_fsync_status, C_ERR);
        EXPECT_EQ(server.aof_bio_fsync_error_gen, 9ULL);
        EXPECT_EQ(server.fsynced_reploff_pending, 300);
    }
}
