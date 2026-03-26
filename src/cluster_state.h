#ifndef CLUSTER_STATE_H
#define CLUSTER_STATE_H

#include "cluster.h"

/* Node accessor used by protocol implementations and description generation. */
char *humanNodename(clusterNode *node);

#endif /* CLUSTER_STATE_H */
