/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "cgroup.h"
#include "sds.h"
}

#ifdef __linux__
typedef struct {
    char root[64];
    sds mount;
    sds membership;
    sds mounts;
} cgroupFixture;

static void writeFile(const char *path, const char *value) {
    FILE *fp = fopen(path, "w");
    ASSERT_NE(fp, (FILE *)NULL);
    EXPECT_GE(fputs(value, fp), 0);
    EXPECT_EQ(fclose(fp), 0);
}

static void writeLimit(cgroupFixture *fixture, const char *name, const char *value) {
    sds path = sdscatprintf(sdsempty(), "%s/%s", fixture->mount, name);
    if (value)
        writeFile(path, value);
    else
        unlink(path);
    sdsfree(path);
}

static void initFixture(cgroupFixture *fixture) {
    strcpy(fixture->root, "/tmp/valkey-cgroup-XXXXXX");
    ASSERT_NE(mkdtemp(fixture->root), (char *)NULL);
    fixture->mount = sdscatprintf(sdsempty(), "%s/mount space", fixture->root);
    fixture->membership = sdscatprintf(sdsempty(), "%s/cgroup", fixture->root);
    fixture->mounts = sdscatprintf(sdsempty(), "%s/mountinfo", fixture->root);
    ASSERT_EQ(mkdir(fixture->mount, 0700), 0);
    sds path = sdscatprintf(sdsempty(), "%s/parent", fixture->mount);
    EXPECT_EQ(mkdir(path, 0700), 0);
    path = sdscat(path, "/leaf");
    EXPECT_EQ(mkdir(path, 0700), 0);
    sdsfree(path);
}

static void setMount(cgroupFixture *fixture, int version, const char *root) {
    /* The fixture mount has a space, exercising mountinfo unescaping. */
    sds escaped = sdsempty();
    for (const char *p = fixture->mount; *p; p++) {
        escaped = *p == ' ' ? sdscat(escaped, "\\040") : sdscatlen(escaped, p, 1);
    }
    sds line = sdscatprintf(sdsempty(), "17 1 0:30 %s %s rw - %s cgroup %s\n", root, escaped,
                            version == 2 ? "cgroup2" : "cgroup", version == 2 ? "rw" : "rw,memory");
    writeFile(fixture->mounts, line);
    sdsfree(line);
    sdsfree(escaped);
}

static void freeFixture(cgroupFixture *fixture) {
    const char *files[] = {"memory.max", "parent/memory.max", "parent/leaf/memory.max",
                           "memory.limit_in_bytes", "parent/memory.limit_in_bytes",
                           "parent/leaf/memory.limit_in_bytes", "parent/leaf/memory.stat"};
    for (unsigned int i = 0; i < sizeof(files) / sizeof(files[0]); i++) writeLimit(fixture, files[i], NULL);
    sds path = sdscatprintf(sdsempty(), "%s/parent/leaf", fixture->mount);
    EXPECT_EQ(rmdir(path), 0);
    sdsrange(path, 0, -6);
    EXPECT_EQ(rmdir(path), 0);
    sdsfree(path);
    EXPECT_EQ(rmdir(fixture->mount), 0);
    unlink(fixture->membership);
    unlink(fixture->mounts);
    EXPECT_EQ(rmdir(fixture->root), 0);
    sdsfree(fixture->mount);
    sdsfree(fixture->membership);
    sdsfree(fixture->mounts);
}

TEST(CgroupMemory, V2HierarchyAndLiveChanges) {
    cgroupFixture fixture;
    initFixture(&fixture);
    writeFile(fixture.membership, "0::/parent/leaf\n");
    setMount(&fixture, 2, "/");
    writeLimit(&fixture, "parent/leaf/memory.max", "max\n");
    writeLimit(&fixture, "parent/memory.max", "268435456\n");
    unsigned long long limit = 0;
    EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), 0);
    EXPECT_EQ(limit, 268435456ULL);
    writeLimit(&fixture, "parent/memory.max", "536870912\n");
    EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), 0);
    EXPECT_EQ(limit, 536870912ULL);
    writeLimit(&fixture, "parent/leaf/memory.max", "134217728\n");
    EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), 0);
    EXPECT_EQ(limit, 134217728ULL);
    writeLimit(&fixture, "parent/memory.max", "max\n");
    writeLimit(&fixture, "parent/leaf/memory.max", "max\n");
    EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), 0);
    EXPECT_EQ(limit, ULLONG_MAX);
    freeFixture(&fixture);
}

TEST(CgroupMemory, MountRootsAndNamespaces) {
    const char *memberships[] = {"0::/tenant/parent/leaf\n", "0::/parent/leaf\n", "0::/\n"};
    cgroupFixture fixture;
    initFixture(&fixture);
    setMount(&fixture, 2, "/tenant");
    writeLimit(&fixture, "memory.max", "268435456\n");
    for (unsigned int i = 0; i < sizeof(memberships) / sizeof(memberships[0]); i++) {
        writeFile(fixture.membership, memberships[i]);
        unsigned long long limit = 0;
        EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), 0);
        EXPECT_EQ(limit, 268435456ULL);
    }
    freeFixture(&fixture);
}

TEST(CgroupMemory, V1AndHybridHierarchies) {
    cgroupFixture fixture;
    initFixture(&fixture);
    writeFile(fixture.membership, "0::/unified\n4:cpu,cpuacct:/other\n5:memory:/parent/leaf\n");
    setMount(&fixture, 1, "/");
    writeLimit(&fixture, "parent/leaf/memory.limit_in_bytes", "536870912\n");
    writeLimit(&fixture, "parent/memory.limit_in_bytes", "268435456\n");
    unsigned long long limit = 0;
    EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), 0);
    EXPECT_EQ(limit, 268435456ULL);
    /* memory.stat includes limits imposed above a subtree mount. */
    writeLimit(&fixture, "parent/leaf/memory.stat", "cache 0\nhierarchical_memory_limit 134217728\nrss 0\n");
    EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), 0);
    EXPECT_EQ(limit, 134217728ULL);
    /* On older kernels, disabled hierarchical accounting is reflected here. */
    writeLimit(&fixture, "parent/leaf/memory.stat", "hierarchical_memory_limit 536870912\n");
    EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), 0);
    EXPECT_EQ(limit, 536870912ULL);
    writeLimit(&fixture, "parent/leaf/memory.limit_in_bytes", "9223372036854771712\n");
    writeLimit(&fixture, "parent/memory.limit_in_bytes", "9223372036854771712\n");
    writeLimit(&fixture, "parent/leaf/memory.stat", "hierarchical_memory_limit 9223372036854771712\n");
    EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), 0);
    EXPECT_EQ(limit, ULLONG_MAX);
    freeFixture(&fixture);
}

TEST(CgroupMemory, MultipleMountsIncludeVisibleAncestors) {
    cgroupFixture fixture;
    initFixture(&fixture);
    writeFile(fixture.membership, "0::/parent/leaf\n");
    sds mounts = sdscatprintf(sdsempty(),
                              "17 1 0:30 /parent %s/mount\\040space/parent rw - cgroup2 cgroup rw\n"
                              "18 1 0:30 / %s/mount\\040space rw - cgroup2 cgroup rw\n",
                              fixture.root, fixture.root);
    writeFile(fixture.mounts, mounts);
    sdsfree(mounts);
    writeLimit(&fixture, "memory.max", "134217728\n");
    writeLimit(&fixture, "parent/memory.max", "268435456\n");
    writeLimit(&fixture, "parent/leaf/memory.max", "max\n");
    unsigned long long limit = 0;
    EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), 0);
    EXPECT_EQ(limit, 134217728ULL);
    freeFixture(&fixture);
}

TEST(CgroupMemory, ReadFailuresDoNotRemoveLimit) {
    cgroupFixture fixture;
    initFixture(&fixture);
    writeFile(fixture.membership, "0::/parent/leaf\n");
    setMount(&fixture, 2, "/");
    sds path = sdscatprintf(sdsempty(), "%s/parent/leaf/memory.max", fixture.mount);
    EXPECT_EQ(mkdir(path, 0700), 0);
    unsigned long long limit = 268435456;
    EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), -1);
    EXPECT_EQ(limit, 268435456ULL);
    EXPECT_EQ(rmdir(path), 0);
    sdsfree(path);
    unlink(fixture.membership);
    EXPECT_EQ(mkdir(fixture.membership, 0700), 0);
    EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), -1);
    EXPECT_EQ(limit, 268435456ULL);
    EXPECT_EQ(rmdir(fixture.membership), 0);
    EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), 1);
    EXPECT_EQ(limit, ULLONG_MAX);
    freeFixture(&fixture);
}

TEST(CgroupMemory, InvalidLimitsPreservePreviousValue) {
    const char *invalid[] = {"", "-1\n", "12garbage\n", "18446744073709551616\n", "maximal\n"};
    cgroupFixture fixture;
    initFixture(&fixture);
    writeFile(fixture.membership, "0::/parent/leaf\n");
    setMount(&fixture, 2, "/");
    writeLimit(&fixture, "parent/leaf/memory.max", "268435456\n");
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        writeLimit(&fixture, "parent/memory.max", invalid[i]);
        unsigned long long limit = 123;
        EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), -1);
        EXPECT_EQ(limit, 123ULL);
    }
    freeFixture(&fixture);
}

TEST(CgroupMemory, MissingControllerAndZeroLimit) {
    cgroupFixture fixture;
    initFixture(&fixture);
    writeFile(fixture.membership, "0::/parent/leaf\n");
    setMount(&fixture, 2, "/");
    unsigned long long limit = 0;
    EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), 0);
    EXPECT_EQ(limit, ULLONG_MAX);
    writeLimit(&fixture, "parent/leaf/memory.max", "0\n");
    EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), 0);
    EXPECT_EQ(limit, 0ULL);
    writeFile(fixture.membership, "3:cpu,cpuacct:/parent/leaf\n");
    EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), 1);
    EXPECT_EQ(limit, ULLONG_MAX);
    writeFile(fixture.membership, "0::/parent/../leaf\n");
    EXPECT_EQ(cgroupReadMemoryLimit(fixture.membership, fixture.mounts, &limit), -1);
    freeFixture(&fixture);
}
#else
TEST(CgroupMemory, NonLinuxFallback) {
    unsigned long long limit = 0;
    EXPECT_EQ(cgroupGetMemoryLimit(&limit), 1);
    EXPECT_EQ(limit, ULLONG_MAX);
}
#endif
