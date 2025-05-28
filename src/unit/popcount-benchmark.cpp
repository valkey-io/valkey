#include <benchmark/benchmark.h>
#include <random>
#include <vector>
#include <cstdint>
#include <cstring>
#include <immintrin.h>

//constexpr size_t DATA_SIZE = 1 * 1024 * 1024; // 1 MB
//constexpr size_t DATA_SIZE = 10 * 1024 * 1024; // 10 MB
constexpr size_t DATA_SIZE = 100 * 1024 * 1024; // 100 MB
//constexpr size_t DATA_SIZE = 1024 * 1024 * 1024; // 1 GB

static const unsigned char bitsinbyte[256] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 1, 2, 2, 3, 2,
    3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3,
    3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5,
    6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4,
    3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4,
    5, 5, 6, 5, 6, 6, 7, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6,
    6, 7, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8};
    
long long popcountAVX512(void *s, long count) {
    const size_t chunks = count / 64;
    uint8_t *ptr = (uint8_t *)s;
    const uint8_t *end = ptr + count;

    __m512i accumulator = _mm512_setzero_si512();
    for (size_t i = 0; i < chunks; i++, ptr += 64)
    {
        const __m512i v = _mm512_loadu_si512((const __m512i *)ptr);
        const __m512i p = _mm512_popcnt_epi64(v);
        accumulator = _mm512_add_epi64(accumulator, p);
    }

    if (ptr < end)
    {
        const size_t remaining = end - ptr;
        __mmask64 mask = (1ULL << remaining) - 1;
        const __m512i v = _mm512_maskz_loadu_epi8(mask, ptr);
        const __m512i p = _mm512_popcnt_epi64(v);
        accumulator = _mm512_add_epi64(accumulator, p);
    }

    return _mm512_reduce_add_epi64(accumulator);
}

static std::vector<uint8_t> generate_random_data(size_t size) {
    std::vector<uint8_t> data(size);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>(dist(rng));
    }
    return data;
}

static void BM_popcountAVX512(benchmark::State& state) {
    auto data = generate_random_data(state.range(0));
    for (auto _ : state) {
        benchmark::DoNotOptimize(popcountAVX512(data.data(), data.size()));
    }
}
BENCHMARK(BM_popcountAVX512)->Arg(DATA_SIZE);

long long popcountAVX2(void *s, long count) {
    long i = 0;
    unsigned char *p = (unsigned char *)s;
    long long bits = 0;

    /* clang-format off */
    const __m256i lookup = _mm256_setr_epi8(
        /* First Lane [0:127] */
        /* 0 */ 0, /* 1 */ 1, /* 2 */ 1, /* 3 */ 2,
        /* 4 */ 1, /* 5 */ 2, /* 6 */ 2, /* 7 */ 3,
        /* 8 */ 1, /* 9 */ 2, /* a */ 2, /* b */ 3,
        /* c */ 2, /* d */ 3, /* e */ 3, /* f */ 4,

        /* Second Lane [128:255] identical to first lane due to lane isolation in _mm256_shuffle_epi8.
         * For more information, see following URL
         * https://www.intel.com/content/www/us/en/docs/intrinsics-guide/index.html#text=_mm256_shuffle_epi8 */
        /* 0 */ 0, /* 1 */ 1, /* 2 */ 1, /* 3 */ 2,
        /* 4 */ 1, /* 5 */ 2, /* 6 */ 2, /* 7 */ 3,
        /* 8 */ 1, /* 9 */ 2, /* a */ 2, /* b */ 3,
        /* c */ 2, /* d */ 3, /* e */ 3, /* f */ 4);
    /* clang-format on */
    const __m256i low_mask = _mm256_set1_epi8(0x0f);
    __m256i acc = _mm256_setzero_si256();

/* Count 32 bytes per iteration. */
#define ITER_32_BYTES                                                             \
    {                                                                             \
        const __m256i vec = _mm256_loadu_si256((const __m256i *)(p + i));         \
        const __m256i lo = _mm256_and_si256(vec, low_mask);                       \
        const __m256i hi = _mm256_and_si256(_mm256_srli_epi16(vec, 4), low_mask); \
        const __m256i popcnt1 = _mm256_shuffle_epi8(lookup, lo);                  \
        const __m256i popcnt2 = _mm256_shuffle_epi8(lookup, hi);                  \
        local = _mm256_add_epi8(local, popcnt1);                                  \
        local = _mm256_add_epi8(local, popcnt2);                                  \
        i += 32;                                                                  \
    }

    /* We divide the array into the following three parts
     *        Part A         Part B       Part C
     * +-----------------+--------------+---------+
     * | 8 * 32bytes * X |  32bytes * Y | Z bytes |
     * +-----------------+--------------+---------+
     */

    /* Part A: loop unrolling, processing 8 * 32 bytes per iteration. */
    while (i + 8 * 32 <= count) {
        __m256i local = _mm256_setzero_si256();
        ITER_32_BYTES
        ITER_32_BYTES
        ITER_32_BYTES
        ITER_32_BYTES
        ITER_32_BYTES
        ITER_32_BYTES
        ITER_32_BYTES
        ITER_32_BYTES
        acc = _mm256_add_epi64(acc, _mm256_sad_epu8(local, _mm256_setzero_si256()));
    }

    /* Part B: when the remaining data length is less than 8 * 32 bytes,
     * process 32 bytes per iteration. */
    __m256i local = _mm256_setzero_si256();
    while (i + 32 <= count) {
        ITER_32_BYTES;
    }
    acc = _mm256_add_epi64(acc, _mm256_sad_epu8(local, _mm256_setzero_si256()));

#undef ITER_32_BYTES

    bits += _mm256_extract_epi64(acc, 0);
    bits += _mm256_extract_epi64(acc, 1);
    bits += _mm256_extract_epi64(acc, 2);
    bits += _mm256_extract_epi64(acc, 3);

    /* Part C: count the remaining bytes. */
    for (; i < count; i++) {
        bits += bitsinbyte[p[i]];
    }

    return bits;
}

static void BM_popcountAVX2(benchmark::State& state) {
    auto data = generate_random_data(state.range(0));
    for (auto _ : state) {
        benchmark::DoNotOptimize(popcountAVX2(data.data(), data.size()));
    }
}
BENCHMARK(BM_popcountAVX2)->Arg(DATA_SIZE);

BENCHMARK_MAIN();
