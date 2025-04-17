#ifndef __ALLOCATOR_DEFRAG_H
#define __ALLOCATOR_DEFRAG_H

#if defined(USE_JEMALLOC)
#include <jemalloc/jemalloc.h>
/* We can enable the server defrag capabilities only if we are using Jemalloc
 * and the version that has the experimental.utilization namespace in mallctl . */
#if (defined(JEMALLOC_VERSION_MAJOR) &&                                                                 \
     (JEMALLOC_VERSION_MAJOR > 5 ||                                                                     \
      (JEMALLOC_VERSION_MAJOR == 5 && JEMALLOC_VERSION_MINOR > 2) ||                                    \
      (JEMALLOC_VERSION_MAJOR == 5 && JEMALLOC_VERSION_MINOR == 2 && JEMALLOC_VERSION_BUGFIX >= 1))) || \
    defined(DEBUG_FORCE_DEFRAG)
#define HAVE_DEFRAG
#endif
#endif

int allocatorDefragInit(void);
void allocatorDefragFree(void *ptr, size_t size);
__attribute__((malloc)) void *allocatorDefragAlloc(size_t size);
unsigned long allocatorDefragGetFragSmallbins(void);
int allocatorShouldDefrag(void *ptr);
float getAllocatorFragmentation(size_t *out_frag_bytes);

#endif /* __ALLOCATOR_DEFRAG_H */
