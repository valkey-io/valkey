/*
 * Shared memory connection type header
 */

#ifndef VALKEY_SHMEM_H
#define VALKEY_SHMEM_H

#include "connection.h"

/* Register the shared memory connection type */
int valkeyRegisterShmemConnectionType(void);

#endif /* VALKEY_SHMEM_H */
