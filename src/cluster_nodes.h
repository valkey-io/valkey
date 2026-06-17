#ifndef CLUSTER_NODES_H
#define CLUSTER_NODES_H

#include "cluster.h"

/* Node address string: ip:port@cport[,hostname][,aux=val]* */
sds clusterNodeAppendAddressString(sds s, clusterNode *node, int tls_primary);
sds clusterNodeAppendAddressStringNoShardId(sds s, clusterNode *node, int tls_primary);
int clusterNodeParseAddressString(clusterNode *n, char *str);

/* Node description / serialization. */
sds clusterGenNodeDescription(client *c, clusterNode *node, int tls_primary);
sds clusterGenNodesDescription(client *c, int filter, int tls_primary);
sds representClusterNodeFlags(sds ci, uint16_t flags);
void clusterGenNodesSlotsInfo(int filter);
void clusterFreeNodesSlotsInfo(clusterNode *n);

/* Cluster config file (nodes.conf) persistence. */
int clusterLoadConfig(char *filename);
int clusterLoadNodeLine(sds *argv, int argc, dict *nodes_seen);
int clusterSaveConfig(int do_fsync);
void clusterSaveConfigOrDie(int do_fsync);
void clusterSaveConfigOrLog(int do_fsync);
int clusterLockConfig(char *filename);

#endif /* CLUSTER_NODES_H */
