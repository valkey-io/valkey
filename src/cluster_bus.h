#ifndef CLUSTER_BUS_H
#define CLUSTER_BUS_H

#include <stdbool.h>
#include <stdint.h>

typedef char *sds;
struct serverObject;
struct client;

/* Interface for cluster bus protocol implementations.
 * Only includes operations that code outside the protocol
 * implementation needs to call. Internal functions (e.g.
 * message building, gossip processing) stay private to
 * the implementation. */
typedef struct clusterBusType {
    /* Lifecycle — called from server.c */
    void (*init)(void);
    void (*initLast)(void);
    void (*cron)(void);
    void (*beforeSleep)(void);
    void (*handleServerShutdown)(bool auto_failover);

    /* Config updates — called from config.c */
    void (*updateMyselfFlags)(void);
    void (*updateMyselfIp)(void);
    void (*updateMyselfHostname)(void);
    void (*updateMyselfAnnouncedPorts)(void);
    void (*updateMyselfHumanNodename)(void);
    void (*updateMyselfClientIpV4)(void);
    void (*updateMyselfClientIpV6)(void);
    void (*updateMyselfAvailabilityZone)(void);

    /* Message propagation — called from pubsub.c, module.c */
    void (*propagatePublish)(struct serverObject *channel, struct serverObject *message, int sharded);
    int (*sendModuleMessage)(const char *target, uint64_t module_id, uint8_t type, const char *payload, uint32_t len);

    /* Failover — called from replication.c */
    int (*allowFailoverCmd)(struct client *c);
    void (*promoteSelfToPrimary)(void);
    long long (*manualFailoverTimeLimit)(void);

    /* Info and stats — called from server.c, networking.c, config.c */
    unsigned long (*getConnectionsCount)(void);
    void (*resetStats)(void);
    sds (*appendInfoFields)(sds info);
    int (*getFailureReportsCount)(clusterNode *node);

    /* Slot ownership changes — called from cluster commands and slot migration.
     * Assigns or unassigns slots specified by an array of slot ranges. If
     * target is non-NULL, slots are assigned to target. If target is NULL,
     * slots are unassigned. If the target is myself and the slots were being
     * imported, the implementation handles finalization (e.g. epoch bump and
     * broadcast in the legacy protocol). The ctx pointer is passed through
     * to the callback, which is called when the change is applied.
     * TODO: When the callback is asynchronous, the caller must block the
     * client so the server can continue processing other clients while
     * waiting for the consensus commit. */
    void (*slotChange)(slotRange *ranges, int numranges, clusterNode *target, void *ctx, void (*callback)(void *ctx, int success));

    /* Clean up any protocol-specific failover state. Called when the node's
     * role changes and any in-progress failover state is no longer relevant. */
    void (*cleanupFailoverState)(void);

    /* Node management — called from cluster commands */
    int (*forgetNode)(const char *node_id, size_t id_len);
    void (*setReplicaOf)(clusterNode *primary);
    void (*failover)(client *c, int force, int takeover);
    void (*meet)(client *c);
    void (*bumpEpoch)(client *c);
    void (*setConfigEpoch)(client *c);
    void (*resetCluster)(client *c);

    /* Protocol-specific command handling — called from cluster.c, debug.c */
    int (*handleSpecialCommand)(struct client *c);
    int (*handleDebugCommand)(struct client *c);
    const char **(*extendedHelp)(void);
    const char **(*debugExtendedHelp)(void);
} clusterBusType;

extern clusterBusType *clusterCurrentBus;

#endif /* CLUSTER_BUS_H */
