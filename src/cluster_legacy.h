#ifndef CLUSTER_LEGACY_H
#define CLUSTER_LEGACY_H

#include <stdint.h>
#include "cluster_state.h"

#define CLUSTER_PORT_INCR 10000 /* Cluster port = baseport + PORT_INCR */

/* The following defines are amount of time, sometimes expressed as
 * multipliers of the node timeout value (when ending with MULT). */
#define CLUSTER_MF_PAUSE_MULT 2 /* Primary pause manual failover mult. */

/* clusterState todo_before_sleep flags. */
#define CLUSTER_TODO_HANDLE_FAILOVER (1 << 0)
#define CLUSTER_TODO_UPDATE_STATE (1 << 1)
#define CLUSTER_TODO_SAVE_CONFIG (1 << 2)
#define CLUSTER_TODO_FSYNC_CONFIG (1 << 3)
#define CLUSTER_TODO_HANDLE_MANUALFAILOVER (1 << 4)
#define CLUSTER_TODO_BROADCAST_ALL (1 << 5)
#define CLUSTER_TODO_HANDLE_SLOT_MIGRATION (1 << 6)

#endif // CLUSTER_LEGACY_H
