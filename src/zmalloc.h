#ifndef __ZMALLOC_H
#define __ZMALLOC_H

#include <stddef.h>

/* Double expansion needed for stringification of macro values. */
#define __xstr(s) __str(s)
#define __str(s) #s

#if defined(USE_TCMALLOC)
#define ZMALLOC_LIB ("tcmalloc-" __xstr(TC_VERSION_MAJOR) "." __xstr(TC_VERSION_MINOR))
#include <gperftools/tcmalloc.h>
#if (TC_VERSION_MAJOR == 1 && TC_VERSION_MINOR >= 6) || (TC_VERSION_MAJOR > 1)
#define HAVE_MALLOC_SIZE 1
#define zmalloc_size(p) tc_malloc_size(p)
#else
#error "Newer version of tcmalloc required"
#endif

#elif defined(USE_JEMALLOC)
#define ZMALLOC_LIB \
    ("jemalloc-" __xstr(JEMALLOC_VERSION_MAJOR) "." __xstr(JEMALLOC_VERSION_MINOR) "." __xstr(JEMALLOC_VERSION_BUGFIX))
#include <jemalloc/jemalloc.h>
#if (JEMALLOC_VERSION_MAJOR == 2 && JEMALLOC_VERSION_MINOR >= 1) || (JEMALLOC_VERSION_MAJOR > 2)
#define HAVE_MALLOC_SIZE 1
#define zmalloc_size(p) je_malloc_usable_size(p)
#else
#error "Newer version of jemalloc required"
#endif

#elif defined(__APPLE__)
#include <malloc/malloc.h>
#define HAVE_MALLOC_SIZE 1
#define zmalloc_size(p) malloc_size(p)
#endif

/* On native libc implementations, we should still do our best to provide a
 * HAVE_MALLOC_SIZE capability.
 *
 * NO_MALLOC_USABLE_SIZE disables it on all platforms, even if they are
 * known to support it.
 * USE_MALLOC_USABLE_SIZE forces use of malloc_usable_size() regardless
 * of platform.
 */
#ifndef ZMALLOC_LIB
#define ZMALLOC_LIB "libc"
#define USE_LIBC 1

#if !defined(NO_MALLOC_USABLE_SIZE) && (defined(__GLIBC__) || defined(__FreeBSD__) || defined(__DragonFly__) || \
                                        defined(__HAIKU__) || defined(USE_MALLOC_USABLE_SIZE))

/* Includes for malloc_usable_size() */
#ifdef __FreeBSD__
#include <malloc_np.h>
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <malloc.h>
#endif

#define HAVE_MALLOC_SIZE 1
#define zmalloc_size(p) malloc_usable_size(p)

#endif
#endif

/* Includes for malloc_trim(), see zlibc_trim(). */
#if defined(__GLIBC__) && !defined(USE_LIBC)
#include <malloc.h>
#endif

/* The zcalloc symbol is a symbol name already used by zlib, which is defining
 * other names using the "z" prefix specific to zlib. In practice, linking
 * valkey with a static openssl, which itself might depend on a static libz
 * will result in a link time error rejecting multiple symbol definitions.
 */
#define zmalloc    valkey_malloc
#define zcalloc    valkey_calloc
#define zrealloc   valkey_realloc
#define zfree      valkey_free

/* On MSVC, the GNU __attribute__ syntax is not supported.
 * We define it as a variadic macro that discards its arguments.
 */
#ifdef _MSC_VER
#define __attribute__(...) /* nothing */
#endif

/* 'noinline' attribute is intended to prevent the `-Wstringop-overread` warning
 * when using gcc-12 later with LTO enabled. It may be removed once the bug
 * https://gcc.gnu.org/bugzilla/show_bug.cgi?id=96503 is fixed.
 */
__attribute__((malloc, alloc_size(1), noinline)) void *zmalloc(size_t size);
__attribute__((malloc, alloc_size(1), noinline)) void *zcalloc(size_t size);
__attribute__((malloc, alloc_size(1, 2), noinline)) void *zcalloc_num(size_t num, size_t size);
__attribute__((alloc_size(2), noinline)) void *zrealloc(void *ptr, size_t size);
__attribute__((malloc, alloc_size(1), noinline)) void *ztrymalloc(size_t size);
__attribute__((malloc, alloc_size(1), noinline)) void *ztrycalloc(size_t size);
__attribute__((alloc_size(2), noinline)) void *ztryrealloc(void *ptr, size_t size);
void zfree(void *ptr);
void zfree_with_size(void *ptr, size_t size);
void *zmalloc_usable(size_t size, size_t *usable);
void *zcalloc_usable(size_t size, size_t *usable);
void *zrealloc_usable(void *ptr, size_t size, size_t *usable);
void *ztrymalloc_usable(size_t size, size_t *usable);
void *ztrycalloc_usable(size_t size, size_t *usable);
void *ztryrealloc_usable(void *ptr, size_t size, size_t *usable);
__attribute__((malloc)) char *zstrdup(const char *s);
size_t zmalloc_used_memory(void);
void zmalloc_set_oom_handler(void (*oom_handler)(size_t));
size_t zmalloc_get_rss(void);
int zmalloc_get_allocator_info(size_t *allocated, size_t *active, size_t *resident, size_t *retained, size_t *muzzy);
void set_jemalloc_bg_thread(int enable);
int jemalloc_purge(void);
size_t zmalloc_get_private_dirty(long pid);
size_t zmalloc_get_smap_bytes_by_field(char *field, long pid);
size_t zmalloc_get_memory_size(void);
void zlibc_free(void *ptr);
void zlibc_trim(void);
void zmadvise_dontneed(void *ptr, size_t size_hint);

#ifndef HAVE_MALLOC_SIZE
size_t zmalloc_size(void *ptr);
size_t zmalloc_usable_size(void *ptr);
#else
/* If we use 'zmalloc_usable_size()' to obtain additional available memory size
 * and manipulate it, we need to call 'extend_to_usable()' afterwards to ensure
 * the compiler recognizes this extra memory. However, if we use the pointer
 * obtained from z[*]_usable() family functions, there is no need for this step. */
#define zmalloc_usable_size(p) zmalloc_size(p)

/* Derived from https://github.com/systemd/systemd/pull/25688
 * We use zmalloc_usable_size() everywhere to use memory blocks, but that is an
 * abuse of malloc_usable_size(). The extend_to_usable function is a dummy
 * allocator function that notifies the compiler of updated pointer size.
 * The function simply returns the pointer as is; its sole purpose is to carry
 * the alloc_size attribute.
 */
__attribute__((alloc_size(2), noinline)) void *extend_to_usable(void *ptr, size_t size);
#endif

int get_proc_stat_ll(int i, long long *res);

#endif /* __ZMALLOC_H */
