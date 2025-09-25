#ifndef HDR_MALLOC_H__
#define HDR_MALLOC_H__

void *valkey_malloc(size_t size);
void *zcalloc_num(size_t num, size_t size);
void *valkey_realloc(void *ptr, size_t size);
void valkey_free(void *ptr);

#define hdr_malloc valkey_malloc
#define hdr_calloc zcalloc_num
#define hdr_realloc valkey_realloc
#define hdr_free valkey_free
#endif
