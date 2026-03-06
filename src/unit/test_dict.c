#include "../dict.c"
#include "test_help.h"

uint64_t hashCallback(const void *key) {
    return dictGenHashFunction((unsigned char *)key, strlen((char *)key));
}

int compareCallback(const void *key1, const void *key2) {
    int l1, l2;
    l1 = strlen((char *)key1);
    l2 = strlen((char *)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

void freeCallback(void *val) {
    zfree(val);
}

char *stringFromLongLong(long long value) {
    char buf[32];
    int len;
    char *s;

    len = snprintf(buf, sizeof(buf), "%lld", value);
    s = zmalloc(len + 1);
    memcpy(s, buf, len);
    s[len] = '\0';
    return s;
}

dictType BenchmarkDictType = {hashCallback, NULL, compareCallback, freeCallback, NULL, NULL};

#define start_benchmark() start = timeInMilliseconds()
#define end_benchmark(msg)                                      \
    do {                                                        \
        elapsed = timeInMilliseconds() - start;                 \
        printf(msg ": %ld items in %lld ms\n", count, elapsed); \
    } while (0)

static dict *_dict = NULL;
static long j;
static int retval;
static unsigned long new_dict_size, current_dict_used, remain_keys;

#define SAMPLE_NODE_MAX 8

typedef struct {
    int id;
    char name;
    uint64_t forced_hash;
} dictSampleNode;

typedef struct {
    unsigned long picks[SAMPLE_NODE_MAX];
    unsigned long rounds_with_short_pick;
} dictSimulationStats;

static uint64_t sampleNodeHashCallback(const void *key) {
    const dictSampleNode *node = key;
    return node->forced_hash;
}

static int sampleNodeCompareCallback(const void *key1, const void *key2) {
    const dictSampleNode *node1 = key1;
    const dictSampleNode *node2 = key2;
    return node1->id == node2->id;
}

static dictType SampleNodeDictType = {sampleNodeHashCallback, NULL, sampleNodeCompareCallback, NULL, NULL, NULL};

static void minMaxPicks(const unsigned long *picks, int num_nodes, unsigned long *min, unsigned long *max);

static void seedSamplingRandomness(void) {
    const unsigned long long seed = 0x1234abcd5678ULL;
    init_genrand64(seed);
    srandom((unsigned)seed);
}

/* Build the specific 6-node / 8-bucket shape discussed in PR #3258 review:
 * slot 0: F
 * slot 2: D
 * slot 4: A -> E -> C (chained)
 * slot 7: B */
static int createSkewLayout(dictSampleNode nodes[6], dict **out) {
    dict *d = dictCreate(&SampleNodeDictType);
    if (d == NULL) return 1;
    if (dictExpand(d, 8) != DICT_OK) {
        dictRelease(d);
        return 1;
    }

    nodes[0] = (dictSampleNode){.id = 0, .name = 'A', .forced_hash = 4};
    nodes[1] = (dictSampleNode){.id = 1, .name = 'B', .forced_hash = 7};
    nodes[2] = (dictSampleNode){.id = 2, .name = 'C', .forced_hash = 4};
    nodes[3] = (dictSampleNode){.id = 3, .name = 'D', .forced_hash = 2};
    nodes[4] = (dictSampleNode){.id = 4, .name = 'E', .forced_hash = 4};
    nodes[5] = (dictSampleNode){.id = 5, .name = 'F', .forced_hash = 0};

    /* Insert order controls chain order because dictAdd prepends bucket entries. */
    if (dictAdd(d, &nodes[5], &nodes[5]) != DICT_OK) goto fail;
    if (dictAdd(d, &nodes[3], &nodes[3]) != DICT_OK) goto fail;
    if (dictAdd(d, &nodes[2], &nodes[2]) != DICT_OK) goto fail;
    if (dictAdd(d, &nodes[4], &nodes[4]) != DICT_OK) goto fail;
    if (dictAdd(d, &nodes[0], &nodes[0]) != DICT_OK) goto fail;
    if (dictAdd(d, &nodes[1], &nodes[1]) != DICT_OK) goto fail;

    while (dictIsRehashing(d)) dictRehashMicroseconds(d, 100 * 1000);
    if (dictSize(d) != 6 || dictBuckets(d) != 8) goto fail;
    *out = d;
    return 0;

fail:
    dictRelease(d);
    return 1;
}

/* Build an even 8-node / 8-bucket no-chain layout:
 * slot 0:A, 1:B, ... 7:H */
static int createNoChainSpreadLayout(dictSampleNode nodes[8], dict **out) {
    dict *d = dictCreate(&SampleNodeDictType);
    if (d == NULL) return 1;
    if (dictExpand(d, 8) != DICT_OK) {
        dictRelease(d);
        return 1;
    }

    for (int i = 0; i < 8; i++) {
        nodes[i] = (dictSampleNode){.id = i, .name = (char)('A' + i), .forced_hash = (uint64_t)i};
        if (dictAdd(d, &nodes[i], &nodes[i]) != DICT_OK) {
            dictRelease(d);
            return 1;
        }
    }

    while (dictIsRehashing(d)) dictRehashMicroseconds(d, 100 * 1000);
    if (dictSize(d) != 8 || dictBuckets(d) != 8) {
        dictRelease(d);
        return 1;
    }
    *out = d;
    return 0;
}

static int pickNWithSomeKeys(dict *d, int wanted, int num_nodes, unsigned long rounds, unsigned long *picks) {
    dictEntry **sampled = zmalloc(sizeof(dictEntry *) * wanted);
    if (sampled == NULL) return 1;
    memset(picks, 0, sizeof(unsigned long) * num_nodes);

    for (unsigned long iter = 0; iter < rounds; iter++) {
        unsigned int count = dictGetSomeKeys(d, sampled, wanted);
        if (count == 0) {
            zfree(sampled);
            return 1;
        }
        for (unsigned int i = 0; i < count; i++) {
            dictSampleNode *node = dictGetVal(sampled[i]);
            if (node->id < 0 || node->id >= num_nodes) {
                zfree(sampled);
                return 1;
            }
            picks[node->id]++;
        }
    }

    zfree(sampled);
    return 0;
}

static int simulateSomeKeysLoop(dict *d, int wanted, int num_nodes, unsigned long rounds, dictSimulationStats *stats) {
    dictEntry **sampled = zmalloc(sizeof(dictEntry *) * wanted);
    if (sampled == NULL) return 1;

    memset(stats, 0, sizeof(*stats));
    for (unsigned long iter = 0; iter < rounds; iter++) {
        unsigned int count = dictGetSomeKeys(d, sampled, wanted);
        if (count == 0) {
            zfree(sampled);
            return 1;
        }
        if (count < (unsigned int)wanted) stats->rounds_with_short_pick++;
        for (unsigned int i = 0; i < count; i++) {
            dictSampleNode *node = dictGetVal(sampled[i]);
            if (node->id < 0 || node->id >= num_nodes) {
                zfree(sampled);
                return 1;
            }
            stats->picks[node->id]++;
        }
    }

    zfree(sampled);
    return 0;
}

/* Simulate the core of the unstable gossip selection loop:
 * pick random keys, skip duplicates, stop after wanted * 3 attempts. */
static int simulateUnstableLoop(dict *d, int wanted, int num_nodes, unsigned long rounds, dictSimulationStats *stats) {
    memset(stats, 0, sizeof(*stats));

    for (unsigned long iter = 0; iter < rounds; iter++) {
        unsigned char selected[SAMPLE_NODE_MAX] = {0};
        int picked_this_round = 0;
        int maxiterations = wanted * 3;

        while (picked_this_round < wanted && maxiterations--) {
            dictEntry *de = dictGetRandomKey(d);
            if (de == NULL) return 1;
            dictSampleNode *node = dictGetVal(de);
            if (node->id < 0 || node->id >= num_nodes) return 1;
            if (selected[node->id]) continue;
            selected[node->id] = 1;
            stats->picks[node->id]++;
            picked_this_round++;
        }

        if (picked_this_round < wanted) stats->rounds_with_short_pick++;
    }

    return 0;
}

static void printSimulationStats(const char *name, const dictSimulationStats *stats, int num_nodes, unsigned long rounds) {
    unsigned long min_pick, max_pick;
    minMaxPicks(stats->picks, num_nodes, &min_pick, &max_pick);

    printf("%s simulation results:\n", name);
    printf("  per-round inclusion:");
    for (int i = 0; i < num_nodes; i++) {
        printf(" %c=%.4f", 'A' + i, (double)stats->picks[i] / rounds);
    }
    printf("\n");
    printf("  total picks:");
    for (int i = 0; i < num_nodes; i++) {
        printf(" %c=%lu", 'A' + i, stats->picks[i]);
    }
    printf("\n");
    printf("  spread max-min: %lu\n", max_pick - min_pick);
    printf("  rounds with fewer than 3 picks: %lu/%lu\n", stats->rounds_with_short_pick, rounds);
}

static void minMaxPicks(const unsigned long *picks, int num_nodes, unsigned long *min, unsigned long *max) {
    *min = picks[0];
    *max = picks[0];
    for (int i = 1; i < num_nodes; i++) {
        if (picks[i] < *min) *min = picks[i];
        if (picks[i] > *max) *max = picks[i];
    }
}

int test_dictCreate(int argc, char **argv, int flags) {
    _dict = dictCreate(&BenchmarkDictType);

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    monotonicInit(); /* Required for dict tests, that are relying on monotime during dict rehashing. */

    return 0;
}

int test_dictAdd16Keys(int argc, char **argv, int flags) {
    /* Add 16 keys and verify dict resize is ok */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    dictSetResizeEnabled(DICT_RESIZE_ENABLE);
    for (j = 0; j < 16; j++) {
        retval = dictAdd(_dict, stringFromLongLong(j), (void *)j);
        TEST_ASSERT(retval == DICT_OK);
    }
    while (dictIsRehashing(_dict)) dictRehashMicroseconds(_dict, 1000);
    TEST_ASSERT(dictSize(_dict) == 16);
    TEST_ASSERT(dictBuckets(_dict) == 16);

    return 0;
}

int test_dictDisableResize(int argc, char **argv, int flags) {
    /* Use DICT_RESIZE_AVOID to disable the dict resize and pad to (dict_force_resize_ratio * 16) */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Use DICT_RESIZE_AVOID to disable the dict resize, and pad
     * the number of keys to (dict_force_resize_ratio * 16), so we can satisfy
     * dict_force_resize_ratio in next test. */
    dictSetResizeEnabled(DICT_RESIZE_AVOID);
    for (j = 16; j < (long)dict_force_resize_ratio * 16; j++) {
        retval = dictAdd(_dict, stringFromLongLong(j), (void *)j);
        TEST_ASSERT(retval == DICT_OK);
    }
    current_dict_used = dict_force_resize_ratio * 16;
    TEST_ASSERT(dictSize(_dict) == current_dict_used);
    TEST_ASSERT(dictBuckets(_dict) == 16);

    return 0;
}

int test_dictAddOneKeyTriggerResize(int argc, char **argv, int flags) {
    /* Add one more key, trigger the dict resize */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    retval = dictAdd(_dict, stringFromLongLong(current_dict_used), (void *)(current_dict_used));
    TEST_ASSERT(retval == DICT_OK);
    current_dict_used++;
    new_dict_size = 1UL << dictNextExp(current_dict_used);
    TEST_ASSERT(dictSize(_dict) == current_dict_used);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[0]) == 16);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[1]) == new_dict_size);

    /* Wait for rehashing. */
    dictSetResizeEnabled(DICT_RESIZE_ENABLE);
    while (dictIsRehashing(_dict)) dictRehashMicroseconds(_dict, 1000);
    TEST_ASSERT(dictSize(_dict) == current_dict_used);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[0]) == new_dict_size);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[1]) == 0);

    return 0;
}

int test_dictDeleteKeys(int argc, char **argv, int flags) {
    /* Delete keys until we can trigger shrink in next test */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Delete keys until we can satisfy (1 / HASHTABLE_MIN_FILL) in the next test. */
    for (j = new_dict_size / HASHTABLE_MIN_FILL + 1; j < (long)current_dict_used; j++) {
        char *key = stringFromLongLong(j);
        retval = dictDelete(_dict, key);
        zfree(key);
        TEST_ASSERT(retval == DICT_OK);
    }
    current_dict_used = new_dict_size / HASHTABLE_MIN_FILL + 1;
    TEST_ASSERT(dictSize(_dict) == current_dict_used);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[0]) == new_dict_size);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[1]) == 0);

    return 0;
}

int test_dictDeleteOneKeyTriggerResize(int argc, char **argv, int flags) {
    /* Delete one more key, trigger the dict resize */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    current_dict_used--;
    char *key = stringFromLongLong(current_dict_used);
    retval = dictDelete(_dict, key);
    zfree(key);
    unsigned long oldDictSize = new_dict_size;
    new_dict_size = 1UL << dictNextExp(current_dict_used);
    TEST_ASSERT(retval == DICT_OK);
    TEST_ASSERT(dictSize(_dict) == current_dict_used);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[0]) == oldDictSize);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[1]) == new_dict_size);

    /* Wait for rehashing. */
    while (dictIsRehashing(_dict)) dictRehashMicroseconds(_dict, 1000);
    TEST_ASSERT(dictSize(_dict) == current_dict_used);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[0]) == new_dict_size);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[1]) == 0);

    return 0;
}

int test_dictEmptyDirAdd128Keys(int argc, char **argv, int flags) {
    /* Empty the dictionary and add 128 keys */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    dictEmpty(_dict, NULL);
    for (j = 0; j < 128; j++) {
        retval = dictAdd(_dict, stringFromLongLong(j), (void *)j);
        TEST_ASSERT(retval == DICT_OK);
    }
    while (dictIsRehashing(_dict)) dictRehashMicroseconds(_dict, 1000);
    TEST_ASSERT(dictSize(_dict) == 128);
    TEST_ASSERT(dictBuckets(_dict) == 128);

    return 0;
}

int test_dictDisableResizeReduceTo3(int argc, char **argv, int flags) {
    /* Use DICT_RESIZE_AVOID to disable the dict resize and reduce to 3 */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Use DICT_RESIZE_AVOID to disable the dict reset, and reduce
     * the number of keys until we can trigger shrinking in next test. */
    dictSetResizeEnabled(DICT_RESIZE_AVOID);
    remain_keys = DICTHT_SIZE(_dict->ht_size_exp[0]) / (HASHTABLE_MIN_FILL * dict_force_resize_ratio) + 1;
    for (j = remain_keys; j < 128; j++) {
        char *key = stringFromLongLong(j);
        retval = dictDelete(_dict, key);
        zfree(key);
        TEST_ASSERT(retval == DICT_OK);
    }
    current_dict_used = remain_keys;
    TEST_ASSERT(dictSize(_dict) == remain_keys);
    TEST_ASSERT(dictBuckets(_dict) == 128);

    return 0;
}

int test_dictDeleteOneKeyTriggerResizeAgain(int argc, char **argv, int flags) {
    /* Delete one more key, trigger the dict resize */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    current_dict_used--;
    char *key = stringFromLongLong(current_dict_used);
    retval = dictDelete(_dict, key);
    zfree(key);
    new_dict_size = 1UL << dictNextExp(current_dict_used);
    TEST_ASSERT(retval == DICT_OK);
    TEST_ASSERT(dictSize(_dict) == current_dict_used);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[0]) == 128);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[1]) == new_dict_size);

    /* Wait for rehashing. */
    dictSetResizeEnabled(DICT_RESIZE_ENABLE);
    while (dictIsRehashing(_dict)) dictRehashMicroseconds(_dict, 1000);
    TEST_ASSERT(dictSize(_dict) == current_dict_used);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[0]) == new_dict_size);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[1]) == 0);

    /* This is the last one, restore to original state */
    dictRelease(_dict);

    return 0;
}

int test_dictBenchmark(int argc, char **argv, int flags) {
    long j;
    long long start, elapsed;
    int retval;
    dict *dict = dictCreate(&BenchmarkDictType);
    long count = 0;
    int accurate = (flags & UNIT_TEST_ACCURATE);

    if (argc == 4) {
        if (accurate) {
            count = 5000000;
        } else {
            count = strtol(argv[3], NULL, 10);
        }
    } else {
        count = 5000;
    }

    monotonicInit(); /* Required for dict tests, that are relying on monotime during dict rehashing. */

    start_benchmark();
    for (j = 0; j < count; j++) {
        retval = dictAdd(dict, stringFromLongLong(j), (void *)j);
        TEST_ASSERT(retval == DICT_OK);
    }
    end_benchmark("Inserting");
    TEST_ASSERT((long)dictSize(dict) == count);

    /* Wait for rehashing. */
    while (dictIsRehashing(dict)) {
        dictRehashMicroseconds(dict, 100 * 1000);
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict, key);
        TEST_ASSERT(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict, key);
        TEST_ASSERT(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements (2nd round)");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(rand() % count);
        dictEntry *de = dictFind(dict, key);
        TEST_ASSERT(de != NULL);
        zfree(key);
    }
    end_benchmark("Random access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        dictEntry *de = dictGetRandomKey(dict);
        TEST_ASSERT(de != NULL);
    }
    end_benchmark("Accessing random keys");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(rand() % count);
        key[0] = 'X';
        dictEntry *de = dictFind(dict, key);
        TEST_ASSERT(de == NULL);
        zfree(key);
    }
    end_benchmark("Accessing missing");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        retval = dictDelete(dict, key);
        TEST_ASSERT(retval == DICT_OK);
        key[0] += 17; /* Change first number to letter. */
        retval = dictAdd(dict, key, (void *)j);
        TEST_ASSERT(retval == DICT_OK);
    }
    end_benchmark("Removing and adding");
    dictRelease(dict);
    return 0;
}

/* Benchmark comparing the gossip-style random sampling approaches:
 * 1) Calling dictGetRandomKey() in a loop (old approach)
 * 2) Calling dictGetSomeKeys() once as a batch (new approach)
 *
 * Both simulate selecting `wanted` random entries from a dict of `count`
 * entries, mirroring the gossip field population in cluster_legacy.c. */
int test_dictBenchmarkGetRandomKeyVsGetSomeKeys(int argc, char **argv, int flags) {
    long long start, elapsed;
    int accurate = (flags & UNIT_TEST_ACCURATE);
    long count = accurate ? 1000000 : 2000;
    long iterations = accurate ? 1000 : 50000;

    UNUSED(argc);
    UNUSED(argv);

    monotonicInit();

    /* Build a dict with `count` entries. */
    dict *d = dictCreate(&BenchmarkDictType);
    for (long i = 0; i < count; i++) {
        int retval = dictAdd(d, stringFromLongLong(i), (void *)i);
        TEST_ASSERT(retval == DICT_OK);
    }
    while (dictIsRehashing(d)) dictRehashMicroseconds(d, 100 * 1000);

    /* `wanted` is 10% of total nodes, matching the default gossip config. */
    int wanted = (int)(count * 0.10);

    /* --- Approach 1: dictGetRandomKey() in a retry loop --- */
    start_benchmark();
    for (long iter = 0; iter < iterations; iter++) {
        int gossipcount = 0;
        int maxiterations_inner = wanted * 3;
        while (gossipcount < wanted && maxiterations_inner--) {
            dictEntry *de = dictGetRandomKey(d);
            TEST_ASSERT(de != NULL);
            /* In real code we'd filter here; just count to simulate the work. */
            gossipcount++;
        }
        TEST_ASSERT(gossipcount == wanted);
    }
    end_benchmark("dictGetRandomKey loop");
    long long elapsed_random_key = elapsed;

    /* --- Approach 2: dictGetSomeKeys() batch --- */
    int candidates_wanted = wanted + 2;
    dictEntry **candidates = zmalloc(sizeof(dictEntry *) * candidates_wanted);

    start_benchmark();
    for (long iter = 0; iter < iterations; iter++) {
        unsigned int ncandidates = dictGetSomeKeys(d, candidates, candidates_wanted);
        TEST_ASSERT(ncandidates > 0);
        int gossipcount = 0;
        for (unsigned int i = 0; i < ncandidates && gossipcount < wanted; i++) {
            TEST_ASSERT(candidates[i] != NULL);
            gossipcount++;
        }
    }
    end_benchmark("dictGetSomeKeys batch");
    long long elapsed_some_keys = elapsed;

    printf("Speedup: dictGetSomeKeys is %.2fx faster\n",
           elapsed_some_keys > 0 ? (double)elapsed_random_key / (double)elapsed_some_keys : 0.0);

    zfree(candidates);
    dictRelease(d);
    return 0;
}

/* Verify that dictGetSomeKeys can show noticeable skew in the exact adversarial
 * 6-node / 8-bucket layout discussed in PR #3258. */
int test_dictGetSomeKeysSmallClusterLayoutSkew(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);

    const int num_nodes = 6;
    int wanted = 3;
    unsigned long rounds = (flags & UNIT_TEST_ACCURATE) ? 200000 : 50000;
    dictSampleNode nodes[6];
    unsigned long picks[6];
    unsigned long min_pick, max_pick;

    dict *d = NULL;
    TEST_ASSERT(createSkewLayout(nodes, &d) == 0);
    seedSamplingRandomness();
    TEST_ASSERT(pickNWithSomeKeys(d, wanted, num_nodes, rounds, picks) == 0);
    minMaxPicks(picks, num_nodes, &min_pick, &max_pick);

    printf("dictGetSomeKeys skew layout picks: A=%lu B=%lu C=%lu D=%lu E=%lu F=%lu\n",
           picks[0], picks[1], picks[2], picks[3], picks[4], picks[5]);
    printf("dictGetSomeKeys skew spread: max-min=%lu (rounds=%lu)\n", max_pick - min_pick, rounds);

    /* In this layout we expect a clear spread: some nodes are selected much
     * more often than others. */
    TEST_ASSERT_MESSAGE("dictGetSomeKeys unexpectedly uniform in adversarial layout",
                        (max_pick - min_pick) > (unsigned long)(rounds * 0.15));

    dictRelease(d);
    return 0;
}

/* Verify that the adversarial layout causes node-specific bias:
 * B (slot 7) is selected substantially less often and D (slot 2) more often
 * than the uniform expectation. */
int test_dictGetSomeKeysNodeSpecificBias(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);

    const int num_nodes = 6;
    int wanted = 3;
    unsigned long rounds = (flags & UNIT_TEST_ACCURATE) ? 200000 : 50000;
    dictSampleNode nodes[6];
    unsigned long picks[6];
    long expected_per_node;
    long deviation_b, deviation_d;

    dict *d = NULL;
    TEST_ASSERT(createSkewLayout(nodes, &d) == 0);

    seedSamplingRandomness();
    TEST_ASSERT(pickNWithSomeKeys(d, wanted, num_nodes, rounds, picks) == 0);

    expected_per_node = (long)((rounds * wanted) / 6);
    deviation_b = expected_per_node - (long)picks[1];
    deviation_d = (long)picks[3] - expected_per_node;

    printf("dictGetSomeKeys expected per node=%ld, B=%lu (dev=%ld), D=%lu (dev=%ld)\n",
           expected_per_node, picks[1], deviation_b, picks[3], deviation_d);

    TEST_ASSERT_MESSAGE("Node B should be significantly under-selected in adversarial layout",
                        deviation_b > (long)(rounds * 0.08));
    TEST_ASSERT_MESSAGE("Node D should be significantly over-selected in adversarial layout",
                        deviation_d > (long)(rounds * 0.08));

    dictRelease(d);
    return 0;
}

/* Side-by-side simulation of the unstable loop vs dictGetSomeKeys in the
 * adversarial 6-node / 8-bucket layout discussed in PR #3258. */
int test_dictGossipSamplingSimulationUnstableVsSomeKeys(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);

    const int num_nodes = 6;
    int wanted = 3;
    unsigned long rounds = (flags & UNIT_TEST_ACCURATE) ? 500000 : 100000;
    dictSampleNode nodes[6];
    dictSimulationStats unstable_stats, some_keys_stats;
    unsigned long min_unstable, max_unstable, min_some_keys, max_some_keys;
    unsigned long unstable_spread, some_keys_spread;

    dict *d = NULL;
    TEST_ASSERT(createSkewLayout(nodes, &d) == 0);

    seedSamplingRandomness();
    TEST_ASSERT(simulateUnstableLoop(d, wanted, num_nodes, rounds, &unstable_stats) == 0);
    seedSamplingRandomness();
    TEST_ASSERT(simulateSomeKeysLoop(d, wanted, num_nodes, rounds, &some_keys_stats) == 0);

    printSimulationStats("unstable", &unstable_stats, num_nodes, rounds);
    printSimulationStats("dictGetSomeKeys", &some_keys_stats, num_nodes, rounds);

    minMaxPicks(unstable_stats.picks, num_nodes, &min_unstable, &max_unstable);
    minMaxPicks(some_keys_stats.picks, num_nodes, &min_some_keys, &max_some_keys);
    unstable_spread = max_unstable - min_unstable;
    some_keys_spread = max_some_keys - min_some_keys;

    TEST_ASSERT_MESSAGE("Expected unstable loop to be more skewed than dictGetSomeKeys in this layout",
                        unstable_spread > some_keys_spread);
    TEST_ASSERT_MESSAGE("Expected unstable loop to sometimes produce fewer picks than wanted",
                        unstable_stats.rounds_with_short_pick > 0);
    TEST_ASSERT_MESSAGE("Expected dictGetSomeKeys to always produce 3 picks in this layout",
                        some_keys_stats.rounds_with_short_pick == 0);

    dictRelease(d);
    return 0;
}

/* Side-by-side simulation when nodes are evenly spread and there are no
 * bucket chains (8 nodes in 8 buckets). */
int test_dictGossipSamplingSimulationUnstableVsSomeKeysNoChainSpread(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);

    const int num_nodes = 8;
    int wanted = 3;
    unsigned long rounds = (flags & UNIT_TEST_ACCURATE) ? 500000 : 100000;
    dictSampleNode nodes[8];
    dictSimulationStats unstable_stats, some_keys_stats;
    unsigned long min_unstable, max_unstable, min_some_keys, max_some_keys;
    unsigned long unstable_spread, some_keys_spread;
    long expected_per_node = (long)((rounds * wanted) / num_nodes);
    long max_dev_unstable = 0, max_dev_some_keys = 0;

    dict *d = NULL;
    TEST_ASSERT(createNoChainSpreadLayout(nodes, &d) == 0);

    seedSamplingRandomness();
    TEST_ASSERT(simulateUnstableLoop(d, wanted, num_nodes, rounds, &unstable_stats) == 0);
    seedSamplingRandomness();
    TEST_ASSERT(simulateSomeKeysLoop(d, wanted, num_nodes, rounds, &some_keys_stats) == 0);

    printSimulationStats("unstable (no-chain spread)", &unstable_stats, num_nodes, rounds);
    printSimulationStats("dictGetSomeKeys (no-chain spread)", &some_keys_stats, num_nodes, rounds);

    minMaxPicks(unstable_stats.picks, num_nodes, &min_unstable, &max_unstable);
    minMaxPicks(some_keys_stats.picks, num_nodes, &min_some_keys, &max_some_keys);
    unstable_spread = max_unstable - min_unstable;
    some_keys_spread = max_some_keys - min_some_keys;

    for (int i = 0; i < num_nodes; i++) {
        long dev_unstable = labs((long)unstable_stats.picks[i] - expected_per_node);
        long dev_some_keys = labs((long)some_keys_stats.picks[i] - expected_per_node);
        if (dev_unstable > max_dev_unstable) max_dev_unstable = dev_unstable;
        if (dev_some_keys > max_dev_some_keys) max_dev_some_keys = dev_some_keys;
    }

    printf("no-chain expected per node=%ld\n", expected_per_node);
    printf("no-chain max deviation: unstable=%ld dictGetSomeKeys=%ld\n", max_dev_unstable, max_dev_some_keys);
    printf("no-chain spread: unstable=%lu dictGetSomeKeys=%lu\n", unstable_spread, some_keys_spread);

    TEST_ASSERT_MESSAGE("Expected dictGetSomeKeys to always produce 3 picks in no-chain layout",
                        some_keys_stats.rounds_with_short_pick == 0);
    TEST_ASSERT_MESSAGE("Expected unstable loop to almost always produce 3 picks in no-chain layout",
                        unstable_stats.rounds_with_short_pick < (unsigned long)(rounds * 0.002));
    TEST_ASSERT_MESSAGE("Unexpectedly high unstable deviation in no-chain layout",
                        max_dev_unstable < (long)(rounds * 0.03));
    TEST_ASSERT_MESSAGE("Unexpectedly high dictGetSomeKeys deviation in no-chain layout",
                        max_dev_some_keys < (long)(rounds * 0.03));

    dictRelease(d);
    return 0;
}
