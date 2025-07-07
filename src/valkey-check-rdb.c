/*
 * Copyright (c) 2016, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "mt19937-64.h"
#include "server.h"
#include "rdb.h"
#include "module.h"
#include "hdr_histogram.h"
#include "fpconv_dtoa.h"

#include <stdarg.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

void createSharedObjects(void);
void rdbLoadProgressCallback(rio *r, const void *buf, size_t len);
void computeDatasetProfile(int dbid, robj *keyobj, robj *o, long long expiretime);

int rdbCheckMode = 0;
int rdbCheckStats = 0;
int rdbCheckOutput = 0;
long long now;

#define LOW_TRACKE_VALUE 1
#define MAX_ELEMENTS_TRACKE 200 * 1024
#define MAX_ELEMENTS_SIZE_TRACKE 1024 * 1024

typedef struct rdbStats {
    size_t type;
    unsigned long keys;
    unsigned long expires;
    unsigned long already_expired;

    unsigned long all_key_size;
    unsigned long all_value_size;

    unsigned long elements;
    unsigned long all_elements_size;

    unsigned long elements_max;
    unsigned long elements_size_max;

    struct hdr_histogram *element_count_histogram;
    struct hdr_histogram *element_size_histogram;
} rdbStats;

struct {
    rio *rio;
    robj *key;                     /* Current key we are reading. */
    int key_type;                  /* Current key type if != -1. */
    unsigned long keys;            /* Number of keys processed. */
    unsigned long expires;         /* Number of keys with an expire. */
    unsigned long already_expired; /* Number of keys already expired. */
    unsigned long lua_scripts;     /* Number of lua scripts. */
    unsigned long functions_num;   /* Number of functions. */
    int doing;                     /* The state while reading the RDB. */
    int error_set;                 /* True if error is populated. */
    char error[1024];
    int databases;
    int format;

    /* stats */
    rdbStats **stats; /* stats group by datatype,encoding,isexpired */
    int stats_num;
    char *stats_output;
} rdbstate;

/* At every loading step try to remember what we were about to do, so that
 * we can log this information when an error is encountered. */
#define RDB_CHECK_DOING_START 0
#define RDB_CHECK_DOING_READ_TYPE 1
#define RDB_CHECK_DOING_READ_EXPIRE 2
#define RDB_CHECK_DOING_READ_KEY 3
#define RDB_CHECK_DOING_READ_OBJECT_VALUE 4
#define RDB_CHECK_DOING_CHECK_SUM 5
#define RDB_CHECK_DOING_READ_LEN 6
#define RDB_CHECK_DOING_READ_AUX 7
#define RDB_CHECK_DOING_READ_MODULE_AUX 8
#define RDB_CHECK_DOING_READ_FUNCTIONS 9

#define OUTPUT_FORMAT_INFO 0
#define OUTPUT_FORMAT_TABLE 1
#define OUTPUT_FORMAT_CSV 2

char *rdb_check_doing_string[] = {
    "start",
    "read-type",
    "read-expire",
    "read-key",
    "read-object-value",
    "check-sum",
    "read-len",
    "read-aux",
    "read-module-aux",
    "read-functions",
};

char *rdb_type_string[] = {
    "string",
    "list-linked",
    "set-hashtable",
    "zset-v1",
    "hash-hashtable",
    "zset-v2",
    "module-pre-release",
    "module-value",
    "",
    "hash-zipmap",
    "list-ziplist",
    "set-intset",
    "zset-ziplist",
    "hash-ziplist",
    "quicklist",
    "stream",
    "hash-listpack",
    "zset-listpack",
    "quicklist-v2",
    "stream-v2",
    "set-listpack",
    "stream-v3",
};

char *type_name[OBJ_TYPE_MAX] = {"string", "list", "set", "zset", "hash", "module", /* module type is special */
                                 "stream"};

/********************** Rdb stats **********************/
void statsRecordCount(size_t eleCount, rdbStats *stats) {
    if (!stats) return;

    stats->elements += eleCount;
    if (stats->elements_max < eleCount) {
        stats->elements_max = eleCount;
    }
    hdr_record_value(stats->element_count_histogram, (int64_t)eleCount);
}

void statsRecordElementSize(size_t eleSize, size_t count, rdbStats *stats) {
    if (!stats) return;

    stats->all_value_size += eleSize * count;

    stats->all_elements_size += eleSize * count;
    if (stats->elements_size_max < eleSize) {
        stats->elements_size_max = eleSize;
    }

    hdr_record_value(stats->element_size_histogram, (int64_t)eleSize);
}

void statsRecordSimple(size_t eleSize, size_t eleCount, rdbStats *stats) {
    statsRecordCount(eleCount, stats);
    statsRecordElementSize(eleSize, eleCount, stats);
}

void statsRecordElementSizeAdd(rdbStats *to, rdbStats *from) {
    if (!to || !from) return;

    to->all_value_size += from->all_value_size;

    to->all_elements_size += from->all_elements_size;
    if (to->elements_size_max < from->elements_size_max) {
        to->elements_size_max = from->elements_size_max;
    }

    hdr_add(to->element_size_histogram, from->element_size_histogram);
}

rdbStats *newRdbStats(size_t type) {
    rdbStats *stats = zcalloc(sizeof(rdbStats));
    if (!stats) return NULL;

    stats->type = type;
    hdr_init(LOW_TRACKE_VALUE, MAX_ELEMENTS_TRACKE, 3, &stats->element_count_histogram);
    hdr_init(LOW_TRACKE_VALUE, MAX_ELEMENTS_SIZE_TRACKE, 3, &stats->element_size_histogram);
    return stats;
}

void deleteRdbStats(rdbStats *stats) {
    hdr_close(stats->element_count_histogram);
    hdr_close(stats->element_size_histogram);
    zfree(stats);
}

rdbStats **initRdbStats(size_t num) {
    rdbStats **tmp = zmalloc(sizeof(struct rdbStats *) * num);

    for (size_t i = 0; i < num; i++) {
        tmp[i] = newRdbStats(i % OBJ_TYPE_MAX);
    }

    return tmp;
}

rdbStats **tryExpandRdbStats(rdbStats **statss, size_t old_num, size_t num) {
    if (old_num >= num) {
        return statss;
    }

    rdbStats **tmp = zrealloc(statss, sizeof(struct rdbStats *) * num);
    serverAssert(tmp != NULL);
    for (size_t i = old_num; i < num; i++) {
        tmp[i] = newRdbStats(i % OBJ_TYPE_MAX);
    }

    return tmp;
}

void freeRdbProfile(rdbStats **statss, size_t num) {
    for (size_t i = 0; i < num; i++) {
        deleteRdbStats(statss[i]);
    }

    zfree(statss);
}

void computeDatasetProfile(int dbid, robj *keyobj, robj *o, long long expiretime) {
    UNUSED(dbid);
    UNUSED(keyobj);
    char buf[128];

    rdbStats *stats = rdbstate.stats[o->type + dbid * OBJ_TYPE_MAX];

    stats->all_key_size += sdslen(keyobj->ptr);
    stats->keys++;

    /* Check if the key already expired. */
    if (expiretime != -1 && expiretime < now)
        stats->already_expired++;
    if (expiretime != -1)
        stats->expires++;

    /* Save the key and associated value */
    if (o->type == OBJ_STRING) {
        statsRecordSimple(stringObjectLen(o), 1, stats);
    } else if (o->type == OBJ_LIST) {
        listTypeIterator *li = listTypeInitIterator(o, 0, LIST_TAIL);
        listTypeEntry entry;
        while (listTypeNext(li, &entry)) {
            robj *eleobj = listTypeGet(&entry);
            statsRecordElementSize(stringObjectLen(eleobj), 1, stats);
            decrRefCount(eleobj);
        }
        listTypeReleaseIterator(li);
        statsRecordCount(listTypeLength(o), stats);
    } else if (o->type == OBJ_SET) {
        setTypeIterator *si = setTypeInitIterator(o);
        sds sdsele;
        while ((sdsele = setTypeNextObject(si)) != NULL) {
            statsRecordElementSize(sdslen(sdsele), 1, stats);
            sdsfree(sdsele);
        }
        setTypeReleaseIterator(si);
        statsRecordCount(setTypeSize(o), stats);
    } else if (o->type == OBJ_ZSET) {
        if (o->encoding == OBJ_ENCODING_LISTPACK) {
            unsigned char *zl = o->ptr;
            unsigned char *eptr, *sptr;
            unsigned char *vstr;
            unsigned int vlen;
            long long vll;
            double score;

            eptr = lpSeek(zl, 0);
            serverAssert(eptr != NULL);
            sptr = lpNext(zl, eptr);
            serverAssert(sptr != NULL);

            while (eptr != NULL) {
                size_t eleLen = 0;

                vstr = lpGetValue(eptr, &vlen, &vll);
                score = zzlGetScore(sptr);

                if (vstr != NULL) {
                    eleLen += vlen;
                } else {
                    ll2string(buf, sizeof(buf), vll);
                    eleLen += strlen(buf);
                }
                const int len = fpconv_dtoa(score, buf);
                buf[len] = '\0';
                eleLen += strlen(buf);
                statsRecordElementSize(eleLen, 1, stats);
                zzlNext(zl, &eptr, &sptr);
            }
            statsRecordCount(lpLength(o->ptr), stats);
        } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = o->ptr;
            hashtableIterator iter;
            hashtableInitIterator(&iter, zs->ht, 0);

            void *next;
            while (hashtableNext(&iter, &next)) {
                zskiplistNode *node = next;
                size_t eleLen = 0;

                const int len = fpconv_dtoa(node->score, buf);
                buf[len] = '\0';
                eleLen += sdslen(node->ele) + strlen(buf);
                statsRecordElementSize(eleLen, 1, stats);
            }
            hashtableResetIterator(&iter);
            statsRecordCount(hashtableSize(zs->ht), stats);
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else if (o->type == OBJ_HASH) {
        hashTypeIterator hi;
        hashTypeInitIterator(o, &hi);
        while (hashTypeNext(&hi) != C_ERR) {
            sds sdsele;
            size_t eleLen = 0;

            sdsele = hashTypeCurrentObjectNewSds(&hi, OBJ_HASH_FIELD);
            eleLen += sdslen(sdsele);
            sdsfree(sdsele);
            sdsele = hashTypeCurrentObjectNewSds(&hi, OBJ_HASH_VALUE);
            eleLen += sdslen(sdsele);
            sdsfree(sdsele);

            statsRecordElementSize(eleLen, 1, stats);
        }
        hashTypeResetIterator(&hi);
        statsRecordCount(hashTypeLength(o), stats);
    } else if (o->type == OBJ_STREAM) {
        streamIterator si;
        streamIteratorStart(&si, o->ptr, NULL, NULL, 0);
        streamID id;
        int64_t numfields;

        while (streamIteratorGetID(&si, &id, &numfields)) {
            while (numfields--) {
                unsigned char *field, *value;
                int64_t field_len, value_len;
                streamIteratorGetField(&si, &field, &value, &field_len, &value_len);
                statsRecordElementSize(field_len + value_len, 1, stats);
            }
        }
        streamIteratorStop(&si);
        statsRecordCount(streamLength(o), stats);
    } else if (o->type == OBJ_MODULE) {
        statsRecordCount(1, stats);
    } else {
        serverPanic("Unknown object type");
    }
}

char *stats_field_string[] = {
    "type.name",
    "keys.total",
    "expire_keys.total",
    "already_expired.total",
    "keys.size",
    "keys.value_size",
    "elements.total",
    "elements.size",
    "elements.num.max",
    "elements.num.avg",
    "elements.num.p99",
    "elements.num.p90",
    "elements.num.p50",
    "elements.size.max",
    "elements.size.avg",
    "elements.size.p99",
    "elements.size.p90",
    "elements.size.p50",
    NULL};

void rdbStatsPrintInfo(rdbStats *stats, char *field_string, char *value, size_t value_len) {
    if (!strcasecmp(field_string, "type.name")) {
        snprintf(value, value_len, "%s", type_name[stats->type]);
    } else if (!strcasecmp(field_string, "keys.total")) {
        snprintf(value, value_len, "%lu", stats->keys);
    } else if (!strcasecmp(field_string, "expire_keys.total")) {
        snprintf(value, value_len, "%lu", stats->expires);
    } else if (!strcasecmp(field_string, "already_expired.total")) {
        snprintf(value, value_len, "%lu", stats->already_expired);
    } else if (!strcasecmp(field_string, "keys.size")) {
        snprintf(value, value_len, "%lu", stats->all_key_size);
    } else if (!strcasecmp(field_string, "keys.value_size")) {
        snprintf(value, value_len, "%lu", stats->all_value_size);
    } else if (!strcasecmp(field_string, "elements.total")) {
        snprintf(value, value_len, "%lu", stats->elements);
    } else if (!strcasecmp(field_string, "elements.size")) {
        snprintf(value, value_len, "%lu", stats->all_elements_size);
    } else if (!strcasecmp(field_string, "elements.num.max")) {
        snprintf(value, value_len, "%lu", stats->elements_max);
    } else if (!strcasecmp(field_string, "elements.num.avg")) {
        snprintf(value, value_len, "%.2lf", (stats->keys > 0 ? (float)stats->elements / (float)stats->keys : 0));
    } else if (!strcasecmp(field_string, "elements.num.p99")) {
        snprintf(value, value_len, "%.2lf", (float)hdr_value_at_percentile(stats->element_count_histogram, 99.0));
    } else if (!strcasecmp(field_string, "elements.num.p90")) {
        snprintf(value, value_len, "%.2lf", (float)hdr_value_at_percentile(stats->element_count_histogram, 90.0));
    } else if (!strcasecmp(field_string, "elements.num.p50")) {
        snprintf(value, value_len, "%.2lf", (float)hdr_value_at_percentile(stats->element_count_histogram, 50.0));
    } else if (!strcasecmp(field_string, "elements.size.max")) {
        snprintf(value, value_len, "%lu", stats->elements_size_max);
    } else if (!strcasecmp(field_string, "elements.size.avg")) {
        snprintf(value, value_len, "%.2lf", (stats->elements > 0 ? (float)stats->all_elements_size / (float)stats->elements : 0));
    } else if (!strcasecmp(field_string, "elements.size.p99")) {
        snprintf(value, value_len, "%.2lf", (float)hdr_value_at_percentile(stats->element_size_histogram, 99.0));
    } else if (!strcasecmp(field_string, "elements.size.p90")) {
        snprintf(value, value_len, "%.2lf", (float)hdr_value_at_percentile(stats->element_size_histogram, 90.0));
    } else if (!strcasecmp(field_string, "elements.size.p50")) {
        snprintf(value, value_len, "%.2lf", (float)hdr_value_at_percentile(stats->element_size_histogram, 50.0));
    }
}

/* Show a few stats collected into 'rdbstate' */
void rdbShowGenericInfo(void) {
    printf("[info] %lu keys read\n", rdbstate.keys);
    printf("[info] %lu expires\n", rdbstate.expires);
    printf("[info] %lu already expired\n", rdbstate.already_expired);
    printf("[info] %lu functions\n", rdbstate.functions_num);
    if (rdbstate.lua_scripts) {
        printf("[info] %lu lua scripts read\n", rdbstate.lua_scripts);
    }

    char buffer[64];
    int stats_fd = -1;
    int saved_stdout = -1;
    if (rdbCheckStats) {
        if (rdbCheckOutput) {
            saved_stdout = dup(STDOUT_FILENO);
            stats_fd = open(rdbstate.stats_output, O_WRONLY | O_CREAT, 0644);
            if (stats_fd == -1) {
                fprintf(stderr, "Cannot open output file: '%s': %s\n", rdbstate.stats_output, strerror(errno));
                exit(1);
            } else {
                dup2(stats_fd, STDOUT_FILENO);
            }
        }

        char field_string[80];
        for (int dbid = 0; dbid <= rdbstate.databases; dbid++) {
            for (size_t i = 0; stats_field_string[i] != NULL; i++) {
                if (rdbstate.format == OUTPUT_FORMAT_TABLE) {
                    snprintf(field_string, sizeof(field_string), "db.%d.%s", dbid, stats_field_string[i]);
                    printf("%-30s", field_string);
                } else if (rdbstate.format == OUTPUT_FORMAT_CSV) {
                    printf("db.%d.%s", dbid, stats_field_string[i]);
                }

                for (size_t obj_type = 0; obj_type < OBJ_TYPE_MAX; obj_type++) {
                    const size_t stats_idx = obj_type + dbid * OBJ_TYPE_MAX;
                    rdbStats *stats = rdbstate.stats[stats_idx];

                    if (rdbstate.format == OUTPUT_FORMAT_INFO) {
                        if (i == 0) continue;
                        snprintf(field_string, sizeof(field_string), "[info] db.%d.type.%s.%s", dbid, type_name[stats->type], stats_field_string[i]);
                        printf("%s:", field_string);
                    } else if (rdbstate.format == OUTPUT_FORMAT_TABLE) {
                        printf("\t");
                    } else if (rdbstate.format == OUTPUT_FORMAT_CSV) {
                        printf(",");
                    }

                    rdbStatsPrintInfo(stats, stats_field_string[i], buffer, sizeof(buffer));
                    if (rdbstate.format == OUTPUT_FORMAT_TABLE) {
                        printf("%-5s", buffer);
                    } else {
                        printf("%s", buffer);
                    }

                    if (rdbstate.format == OUTPUT_FORMAT_INFO) {
                        printf("\n");
                    }
                }
                if (rdbstate.format == OUTPUT_FORMAT_TABLE || rdbstate.format == OUTPUT_FORMAT_CSV)
                    printf("\n");
            }
        }
        if (rdbCheckOutput) {
            dup2(saved_stdout, STDOUT_FILENO);
            close(stats_fd);
            close(saved_stdout);
        }
    }
}

/* Called on RDB errors. Provides details about the RDB and the offset
 * we were when the error was detected. */
void rdbCheckError(const char *fmt, ...) {
    char msg[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    printf("--- RDB ERROR DETECTED ---\n");
    printf("[offset %llu] %s\n", (unsigned long long)(rdbstate.rio ? rdbstate.rio->processed_bytes : 0), msg);
    printf("[additional info] While doing: %s\n", rdb_check_doing_string[rdbstate.doing]);
    if (rdbstate.key) printf("[additional info] Reading key '%s'\n", (char *)rdbstate.key->ptr);
    if (rdbstate.key_type != -1)
        printf("[additional info] Reading type %d (%s)\n", rdbstate.key_type,
               ((unsigned)rdbstate.key_type < sizeof(rdb_type_string) / sizeof(char *))
                   ? rdb_type_string[rdbstate.key_type]
                   : "unknown");
    rdbShowGenericInfo();
}

/* Print information during RDB checking. */
void rdbCheckInfo(const char *fmt, ...) {
    char msg[1024], *msgbuf = msg;
    va_list ap;
    int msglen;

    va_start(ap, fmt);
    msglen = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (msglen > (int)(sizeof(msg) - 1)) {
        msgbuf = sdsnewlen(SDS_NOINIT, sizeof(msg) * 2);
        sdsclear(msgbuf);
        va_start(ap, fmt);
        msgbuf = sdscatvprintf(msgbuf, fmt, ap);
        va_end(ap);
    }

    printf("[offset %llu] %s\n", (unsigned long long)(rdbstate.rio ? rdbstate.rio->processed_bytes : 0), msgbuf);

    if (msgbuf != msg) sdsfree(msgbuf);
}

/* Used inside rdb.c in order to log specific errors happening inside
 * the RDB loading internals. */
void rdbCheckSetError(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(rdbstate.error, sizeof(rdbstate.error), fmt, ap);
    va_end(ap);
    rdbstate.error_set = 1;
}

/* During RDB check we setup a special signal handler for memory violations
 * and similar conditions, so that we can log the offending part of the RDB
 * if the crash is due to broken content. */
void rdbCheckHandleCrash(int sig, siginfo_t *info, void *secret) {
    UNUSED(sig);
    UNUSED(info);
    UNUSED(secret);

    rdbCheckError("Server crash checking the specified RDB file!");
    exit(1);
}

void rdbCheckSetupSignals(void) {
    struct sigaction act;

    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_RESETHAND | SA_SIGINFO;
    act.sa_sigaction = rdbCheckHandleCrash;
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
    sigaction(SIGFPE, &act, NULL);
    sigaction(SIGILL, &act, NULL);
    sigaction(SIGABRT, &act, NULL);
}

/* Check the specified RDB file. Return 0 if the RDB looks sane, otherwise
 * 1 is returned.
 * The file is specified as a filename in 'rdbfilename' if 'fp' is NULL,
 * otherwise the already open file 'fp' is checked. */
int redis_check_rdb(char *rdbfilename, FILE *fp) {
    uint64_t dbid;
    int selected_dbid = -1;
    int type, rdbver;
    char buf[1024];
    long long expiretime;
    static rio rdb; /* Pointed by global struct riostate. */
    struct stat sb;

    now = mstime();
    int closefile = (fp == NULL);
    if (fp == NULL && (fp = fopen(rdbfilename, "r")) == NULL) return 1;

    if (fstat(fileno(fp), &sb) == -1) sb.st_size = 0;

    startLoadingFile(sb.st_size, rdbfilename, RDBFLAGS_NONE);
    rioInitWithFile(&rdb, fp);
    rdbstate.rio = &rdb;
    rdb.update_cksum = rdbLoadProgressCallback;
    if (rioRead(&rdb, buf, 9) == 0) goto eoferr;
    buf[9] = '\0';
    if (memcmp(buf, "REDIS", 5) != 0) {
        rdbCheckError("Wrong signature trying to load DB from file");
        goto err;
    }
    rdbver = atoi(buf + 5);
    if (rdbver < 1 || rdbver > RDB_VERSION) {
        rdbCheckError("Can't handle RDB format version %d", rdbver);
        goto err;
    }

    expiretime = -1;
    while (1) {
        robj *key, *val;

        /* Read type. */
        rdbstate.doing = RDB_CHECK_DOING_READ_TYPE;
        if ((type = rdbLoadType(&rdb)) == -1) goto eoferr;

        /* Handle special types. */
        if (type == RDB_OPCODE_EXPIRETIME) {
            rdbstate.doing = RDB_CHECK_DOING_READ_EXPIRE;
            /* EXPIRETIME: load an expire associated with the next key
             * to load. Note that after loading an expire we need to
             * load the actual type, and continue. */
            expiretime = rdbLoadTime(&rdb);
            expiretime *= 1000;
            if (rioGetReadError(&rdb)) goto eoferr;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EXPIRETIME_MS) {
            /* EXPIRETIME_MS: milliseconds precision expire times introduced
             * with RDB v3. Like EXPIRETIME but no with more precision. */
            rdbstate.doing = RDB_CHECK_DOING_READ_EXPIRE;
            expiretime = rdbLoadMillisecondTime(&rdb, rdbver);
            if (rioGetReadError(&rdb)) goto eoferr;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_FREQ) {
            /* FREQ: LFU frequency. */
            uint8_t byte;
            if (rioRead(&rdb, &byte, 1) == 0) goto eoferr;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_IDLE) {
            /* IDLE: LRU idle time. */
            if (rdbLoadLen(&rdb, NULL) == RDB_LENERR) goto eoferr;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EOF) {
            /* EOF: End of file, exit the main loop. */
            break;
        } else if (type == RDB_OPCODE_SELECTDB) {
            /* SELECTDB: Select the specified database. */
            rdbstate.doing = RDB_CHECK_DOING_READ_LEN;
            if ((dbid = rdbLoadLen(&rdb, NULL)) == RDB_LENERR) goto eoferr;
            rdbCheckInfo("Selecting DB ID %llu", (unsigned long long)dbid);
            selected_dbid = dbid;
            if (selected_dbid > rdbstate.databases) {
                rdbstate.databases = dbid;
            }

            continue; /* Read type again. */
        } else if (type == RDB_OPCODE_RESIZEDB) {
            /* RESIZEDB: Hint about the size of the keys in the currently
             * selected data base, in order to avoid useless rehashing. */
            uint64_t db_size, expires_size;
            rdbstate.doing = RDB_CHECK_DOING_READ_LEN;
            if ((db_size = rdbLoadLen(&rdb, NULL)) == RDB_LENERR) goto eoferr;
            if ((expires_size = rdbLoadLen(&rdb, NULL)) == RDB_LENERR) goto eoferr;
            continue; /* Read type again. */
        } else if (type == RDB_OPCODE_AUX) {
            /* AUX: generic string-string fields. Use to add state to RDB
             * which is backward compatible. Implementations of RDB loading
             * are required to skip AUX fields they don't understand.
             *
             * An AUX field is composed of two strings: key and value. */
            robj *auxkey, *auxval;
            rdbstate.doing = RDB_CHECK_DOING_READ_AUX;
            if ((auxkey = rdbLoadStringObject(&rdb)) == NULL) goto eoferr;
            if ((auxval = rdbLoadStringObject(&rdb)) == NULL) {
                decrRefCount(auxkey);
                goto eoferr;
            }
            if (!strcasecmp(auxkey->ptr, "lua")) {
                /* In older version before 7.0, we may save lua scripts in a replication RDB. */
                rdbstate.lua_scripts++;
            }
            rdbCheckInfo("AUX FIELD %s = '%s'", (char *)auxkey->ptr, (char *)auxval->ptr);
            decrRefCount(auxkey);
            decrRefCount(auxval);
            continue; /* Read type again. */
        } else if (type == RDB_OPCODE_MODULE_AUX) {
            /* AUX: Auxiliary data for modules. */
            uint64_t moduleid, when_opcode, when;
            rdbstate.doing = RDB_CHECK_DOING_READ_MODULE_AUX;
            if ((moduleid = rdbLoadLen(&rdb, NULL)) == RDB_LENERR) goto eoferr;
            if ((when_opcode = rdbLoadLen(&rdb, NULL)) == RDB_LENERR) goto eoferr;
            if ((when = rdbLoadLen(&rdb, NULL)) == RDB_LENERR) goto eoferr;
            if (when_opcode != RDB_MODULE_OPCODE_UINT) {
                rdbCheckError("bad when_opcode");
                goto err;
            }

            char name[10];
            moduleTypeNameByID(name, moduleid);
            rdbCheckInfo("MODULE AUX for: %s", name);

            robj *o = rdbLoadCheckModuleValue(&rdb, name);
            decrRefCount(o);
            continue; /* Read type again. */
        } else if (type == RDB_OPCODE_FUNCTION_PRE_GA) {
            rdbCheckError("Pre-release function format not supported %d", rdbver);
            goto err;
        } else if (type == RDB_OPCODE_FUNCTION2) {
            sds err = NULL;
            rdbstate.doing = RDB_CHECK_DOING_READ_FUNCTIONS;
            if (rdbFunctionLoad(&rdb, rdbver, NULL, 0, &err) != C_OK) {
                rdbCheckError("Failed loading library, %s", err);
                sdsfree(err);
                goto err;
            }
            rdbstate.functions_num++;
            continue;
        } else {
            if (!rdbIsObjectType(type)) {
                rdbCheckError("Invalid object type: %d", type);
                goto err;
            }
            rdbstate.key_type = type;
        }

        /* Read key */
        rdbstate.doing = RDB_CHECK_DOING_READ_KEY;
        if ((key = rdbLoadStringObject(&rdb)) == NULL) goto eoferr;
        rdbstate.key = key;
        rdbstate.keys++;
        /* Read value */
        rdbstate.doing = RDB_CHECK_DOING_READ_OBJECT_VALUE;
        if ((val = rdbLoadObject(type, &rdb, key->ptr, selected_dbid, NULL)) == NULL) goto eoferr;
        if (rdbCheckStats) {
            int max_stats_num = (rdbstate.databases + 1) * OBJ_TYPE_MAX;
            if (max_stats_num > rdbstate.stats_num) {
                rdbstate.stats = tryExpandRdbStats(rdbstate.stats, rdbstate.stats_num, max_stats_num);
                rdbstate.stats_num = max_stats_num;
            }

            computeDatasetProfile(selected_dbid, key, val, expiretime);
        }
        /* Check if the key already expired. */
        if (expiretime != -1 && expiretime < now) rdbstate.already_expired++;
        if (expiretime != -1) rdbstate.expires++;
        rdbstate.key = NULL;
        decrRefCount(key);
        decrRefCount(val);
        rdbstate.key_type = -1;
        expiretime = -1;
    }
    /* Verify the checksum if RDB version is >= 5 */
    if (rdbver >= 5 && server.rdb_checksum) {
        uint64_t cksum, expected = rdb.cksum;

        rdbstate.doing = RDB_CHECK_DOING_CHECK_SUM;
        if (rioRead(&rdb, &cksum, 8) == 0) goto eoferr;
        memrev64ifbe(&cksum);
        if (cksum == 0) {
            rdbCheckInfo("RDB file was saved with checksum disabled: no check performed.");
        } else if (cksum != expected) {
            rdbCheckError("RDB CRC error");
            goto err;
        } else {
            rdbCheckInfo("Checksum OK");
        }
    }

    if (closefile) fclose(fp);
    stopLoading(1);
    return 0;

eoferr: /* unexpected end of file is handled here with a fatal exit */
    if (rdbstate.error_set) {
        rdbCheckError(rdbstate.error);
    } else {
        rdbCheckError("Unexpected EOF reading RDB file");
    }
err:
    if (closefile) fclose(fp);
    stopLoading(0);
    return 1;
}

void parseCheckRdbOptions(int argc, char **argv, FILE *fp) {
    int i = 1;
    int lastarg;

    if (argc < 2 && fp == NULL) {
        goto checkRdbUsage;
    }

    rdbstate.format = OUTPUT_FORMAT_TABLE;

    for (i = 2; i < argc; i++) {
        lastarg = (i == (argc - 1));
        if (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version")) {
            sds version = getVersion();
            printf("valkey-check-rdb %s\n", version);
            sdsfree(version);
            exit(0);
        } else if (!strcmp(argv[i], "--stats")) {
            rdbCheckStats = 1;
        } else if (!strcmp(argv[i], "--output")) {
            rdbstate.stats_output = zstrdup(argv[i + 1]);
            rdbCheckOutput = 1;
            i++;
        } else if (!strcmp(argv[i], "--format")) {
            if (lastarg) goto checkRdbUsage;
            char *format = argv[i + 1];
            if (!strcmp(format, "table")) {
                rdbstate.format = OUTPUT_FORMAT_TABLE;
            } else if (!strcmp(format, "csv")) {
                rdbstate.format = OUTPUT_FORMAT_CSV;
            } else if (!strcmp(format, "info")) {
                rdbstate.format = OUTPUT_FORMAT_INFO;
            } else {
                goto checkRdbUsage;
            }
            i++;
        } else {
            goto checkRdbUsage;
        }
    }

    return;

checkRdbUsage:
    fprintf(stderr, "Usage: %s <rdb-file-name> [--format table|info|csv] [--stats] [--output <file>]\n", argv[0]);
    exit(1);
}

/* RDB check main: called form server.c when the server is executed with the
 * valkey-check-rdb alias, on during RDB loading errors.
 *
 * The function works in two ways: can be called with argc/argv as a
 * standalone executable, or called with a non NULL 'fp' argument if we
 * already have an open file to check. This happens when the function
 * is used to check an RDB preamble inside an AOF file.
 *
 * When called with fp = NULL, the function never returns, but exits with the
 * status code according to success (RDB is sane) or error (RDB is corrupted).
 * Otherwise if called with a non NULL fp, the function returns C_OK or
 * C_ERR depending on the success or failure. */
int redis_check_rdb_main(int argc, char **argv, FILE *fp) {
    parseCheckRdbOptions(argc, argv, fp);

    struct timeval tv;

    gettimeofday(&tv, NULL);
    init_genrand64(((long long)tv.tv_sec * 1000000 + tv.tv_usec) ^ getpid());

    rdbstate.stats = initRdbStats(OBJ_TYPE_MAX);
    rdbstate.stats_num = OBJ_TYPE_MAX;
    rdbstate.databases = 1;
    rdbstate.functions_num = 0;
    rdbstate.lua_scripts = 0;

    /* In order to call the loading functions we need to create the shared
     * integer objects, however since this function may be called from
     * an already initialized server instance, check if we really need to. */
    if (shared.integers[0] == NULL) createSharedObjects();
    server.loading_process_events_interval_bytes = 0;
    server.sanitize_dump_payload = SANITIZE_DUMP_YES;
    rdbCheckMode = 1;
    rdbCheckInfo("Checking RDB file %s", argv[1]);
    rdbCheckSetupSignals();
    int retval = redis_check_rdb(argv[1], fp);
    if (retval == 0) {
        rdbCheckInfo("\\o/ RDB looks OK! \\o/");
        rdbShowGenericInfo();
    }
    if (fp) return (retval == 0) ? C_OK : C_ERR;
    freeRdbProfile(rdbstate.stats, rdbstate.stats_num);
    exit(retval);
}
