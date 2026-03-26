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

    /* Protocol-specific command handling — called from cluster.c, debug.c */
    int (*handleSpecialCommand)(struct client *c);
    int (*handleDebugCommand)(struct client *c);
    const char **(*extendedHelp)(void);
    const char **(*debugExtendedHelp)(void);
} clusterBusType;

extern clusterBusType *clusterCurrentBus;

#endif /* CLUSTER_BUS_H */
