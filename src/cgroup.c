/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cgroup.h"

#ifdef __linux__
#include "sds.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int hasController(const char *controllers, const char *name) {
    size_t len = strlen(name);
    const char *p = controllers;
    while (p && *p) {
        if (!strncmp(p, name, len) && (p[len] == ',' || p[len] == '\0')) return 1;
        p = strchr(p, ',');
        if (p) p++;
    }
    return 0;
}

/* mountinfo escapes whitespace and backslashes using octal sequences. */
static void unescapeMountPath(char *path) {
    char *src = path, *dst = path;
    while (*src) {
        if (*src == '\\' && src[1] >= '0' && src[1] <= '3' &&
            src[2] >= '0' && src[2] <= '7' && src[3] >= '0' && src[3] <= '7') {
            *dst++ = (src[1] - '0') * 64 + (src[2] - '0') * 8 + src[3] - '0';
            src += 4;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static int validCgroupPath(const char *path) {
    if (*path != '/') return 0;
    for (const char *p = path; *p;) {
        p++;
        size_t len = strcspn(p, "/");
        if ((len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.')) return 0;
        p += len;
    }
    return 1;
}

static int parseLimit(char *buf, int version, unsigned long long *limit) {
    size_t len = strcspn(buf, "\n");
    buf[len] = '\0';
    if (version == 2 && !strcmp(buf, "max")) {
        *limit = ULLONG_MAX;
        return 1;
    }
    if (!string2ull(buf, len, limit)) return -1;
    /* v1 represents unlimited as a page-rounded LONG_MAX. */
    long page_size = sysconf(_SC_PAGESIZE);
    if (version == 1 && page_size > 0 &&
        *limit >= (ULLONG_MAX >> 1) / (unsigned long long)page_size * (unsigned long long)page_size)
        *limit = ULLONG_MAX;
    return 1;
}

/* Returns 1 for a value, 0 for a missing controller file, -1 on failure. */
static int readLimit(const char *path, int version, unsigned long long *limit) {
    FILE *fp = fopen(path, "r");
    if (!fp) return errno == ENOENT ? 0 : -1;
    char buf[128];
    int result = -1;
    if (fgets(buf, sizeof(buf), fp)) result = parseLimit(buf, version, limit);
    if (ferror(fp)) result = -1;
    fclose(fp);
    return result;
}

static int readHierarchy(sds path, const char *mount, int version, unsigned long long *limit) {
    unsigned long long result = ULLONG_MAX;
    size_t rootlen = strlen(mount);
    if (version == 1) {
        /* This also accounts for ancestors outside a subtree mount and for
         * memory.use_hierarchy being disabled on older v1 kernels. */
        sds filename = sdscatprintf(sdsempty(), "%s/memory.stat", path);
        FILE *fp = fopen(filename, "r");
        sdsfree(filename);
        if (fp) {
            char buf[256];
            int rc = 0;
            while (fgets(buf, sizeof(buf), fp)) {
                const char *key = "hierarchical_memory_limit ";
                if (!strncmp(buf, key, strlen(key))) {
                    rc = parseLimit(buf + strlen(key), 1, &result);
                    break;
                }
            }
            if (ferror(fp)) rc = -1;
            fclose(fp);
            if (rc == -1) return -1;
            if (rc == 1) {
                *limit = result;
                return 0;
            }
        } else if (errno != ENOENT) {
            return -1;
        }
    }
    while (1) {
        sds filename = sdscatprintf(sdsempty(), "%s/%s", path,
                                    version == 2 ? "memory.max" : "memory.limit_in_bytes");
        unsigned long long value;
        int rc = readLimit(filename, version, &value);
        sdsfree(filename);
        if (rc == -1) return -1;
        if (rc == 1 && value < result) result = value;
        if (sdslen(path) <= rootlen) break;
        char *slash = strrchr(path, '/');
        size_t len = slash == path ? 1 : (size_t)(slash - path);
        sdssetlen(path, len);
        path[len] = '\0';
    }
    *limit = result;
    return 0;
}

/* Paths are parameters so ordinary unit tests can use filesystem fixtures
 * without creating cgroups or depending on the test runner's memory limit.
 * Only publish a result after all visible ancestors were read successfully. */
int cgroupReadMemoryLimit(const char *membership_file, const char *mounts_file, unsigned long long *limit) {
    FILE *fp = fopen(membership_file, "r");
    if (!fp) {
        if (errno != ENOENT) return -1;
        *limit = ULLONG_MAX;
        return 1;
    }
    char *line = NULL;
    size_t capacity = 0;
    sds v1 = NULL, v2 = NULL;
    while (getline(&line, &capacity, fp) != -1) {
        char *controllers = strchr(line, ':');
        if (!controllers) continue;
        char *path = strchr(++controllers, ':');
        if (!path) continue;
        *path++ = '\0';
        path[strcspn(path, "\n")] = '\0';
        if (hasController(controllers, "memory")) {
            sdsfree(v1);
            v1 = sdsnew(path);
        } else if (!strcmp(line, "0:") && !*controllers) {
            sdsfree(v2);
            v2 = sdsnew(path);
        }
    }
    int result = ferror(fp) ? -1 : 0;
    fclose(fp);
    sds membership = v1 ? v1 : v2;
    int version = v1 ? 1 : 2;
    if (result == -1) goto done;
    if (!membership) {
        *limit = ULLONG_MAX;
        result = 1;
        goto done;
    }
    result = -1;
    if (!validCgroupPath(membership)) goto done;
    fp = fopen(mounts_file, "r");
    if (!fp) goto done;
    int found = 0, mounted = 0;
    unsigned long long minimum = ULLONG_MAX;
    /* Prefer mounts whose root contains the membership. If cgroup namespaces
     * made membership relative to a different root, try it mount-relative. */
    for (int relative = 0; relative < 2 && !found; relative++) {
        rewind(fp);
        while (getline(&line, &capacity, fp) != -1) {
            char *separator = strstr(line, " - ");
            if (!separator) continue;
            *separator = '\0';
            char *save = NULL;
            char *type = strtok_r(separator + 3, " ", &save);
            char *source = strtok_r(NULL, " ", &save);
            char *options = strtok_r(NULL, " \n", &save);
            if (!type || !source || !options) continue;
            if (version == 2 ? strcmp(type, "cgroup2") : (strcmp(type, "cgroup") || !hasController(options, "memory"))) continue;
            mounted = 1;
            char *fields[5];
            for (int i = 0; i < 5; i++) fields[i] = strtok_r(i ? NULL : line, " ", &save);
            if (!fields[4]) continue;
            char *root = fields[3], *mount = fields[4];
            unescapeMountPath(root);
            unescapeMountPath(mount);
            if (!validCgroupPath(root) || !validCgroupPath(mount)) continue;
            size_t rootlen = strlen(root);
            const char *suffix = membership;
            if (!relative) {
                if (!strcmp(root, "/")) {
                    suffix = membership;
                } else if (!strncmp(membership, root, rootlen) &&
                           (membership[rootlen] == '/' || membership[rootlen] == '\0')) {
                    suffix = membership + rootlen;
                } else {
                    continue;
                }
            }
            sds path = sdscatprintf(sdsempty(), "%s%s", !strcmp(mount, "/") ? "" : mount, suffix);
            if (!sdslen(path)) path = sdscat(path, "/");
            if (sdslen(path) > 1 && path[sdslen(path) - 1] == '/') sdsrange(path, 0, -2);
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                unsigned long long value;
                found = 1;
                result = readHierarchy(path, mount, version, &value);
                if (result == -1) {
                    sdsfree(path);
                    goto mounts_done;
                }
                if (value < minimum) minimum = value;
            }
            sdsfree(path);
        }
        if (ferror(fp)) break;
    }
    if (ferror(fp)) {
        result = -1;
    } else if (found || !mounted) {
        *limit = minimum;
        result = found ? 0 : 1;
    }
mounts_done:
    fclose(fp);
done:
    free(line);
    sdsfree(v1);
    sdsfree(v2);
    return result;
}
#else
int cgroupReadMemoryLimit(const char *membership_file, const char *mounts_file, unsigned long long *limit) {
    (void)membership_file;
    (void)mounts_file;
    *limit = ULLONG_MAX;
    return 1;
}
#endif

int cgroupGetMemoryLimit(unsigned long long *limit) {
    return cgroupReadMemoryLimit("/proc/self/cgroup", "/proc/self/mountinfo", limit);
}
