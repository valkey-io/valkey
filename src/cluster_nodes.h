#ifndef CLUSTER_NODES_H
#define CLUSTER_NODES_H

#include "cluster.h"

/* Node description / serialization. */
sds clusterGenNodeDescription(client *c, clusterNode *node, int tls_primary);
sds clusterGenNodesDescription(client *c, int filter, int tls_primary);
sds representClusterNodeFlags(sds ci, uint16_t flags);
void clusterGenNodesSlotsInfo(int filter);
void clusterFreeNodesSlotsInfo(clusterNode *n);

/* Cluster config file (nodes.conf) persistence. */
int clusterLoadConfig(char *filename);
int clusterSaveConfig(int do_fsync);
void clusterSaveConfigOrDie(int do_fsync);
void clusterSaveConfigOrLog(int do_fsync);
int clusterLockConfig(char *filename);

#endif /* CLUSTER_NODES_H */
