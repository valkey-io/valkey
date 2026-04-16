#ifndef CLUSTER_BUS_H
#define CLUSTER_BUS_H

#include <stdbool.h>
#include <stdint.h>

typedef char *sds;
struct serverObject;
struct client;
struct clusterLink;

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

    /* Cluster link message handling — called from cluster_link.c */

    /* Validate the message header (first RCVBUF_MIN_READ_LEN bytes)
     * and return the total message length. Returns 0 if invalid. */
    uint32_t (*validateMessageHeader)(char *header);

    /* Process a complete message in link->rcvbuf. Returns 1 on success,
     * 0 if the link is no longer valid (freed). */
    int (*processMessage)(struct clusterLink *link);

    /* Called after an outbound link connection is established. The
     * implementation typically sends an initial message (e.g. PING). */
    void (*postConnect)(struct clusterLink *link);

    /* Called after a config change updates myself's metadata (IP, ports,
     * hostname, flags, etc.). The protocol implementation should arrange
     * for the change to be persisted and propagated. old_flags is the
     * value of myself->flags before the update, so the implementation
     * can detect flag changes that require additional action. */
    void (*onMyselfUpdated)(int old_flags);

    /* Message propagation — called from pubsub.c, module.c */
    void (*propagatePublish)(struct serverObject *channel, struct serverObject *message, int sharded);
    int (*sendModuleMessage)(const char *target, uint64_t module_id, uint8_t type, const char *payload, uint32_t len);

    /* Info and stats — called from server.c, networking.c, config.c */
    unsigned long (*getConnectionsCount)(void);
    void (*resetStats)(void);
    sds (*appendInfoFields)(sds info);
    int (*getFailureReportsCount)(clusterNode *node);

    /* Return protocol-specific per-node ping_sent, pong_received and
     * config_epoch for CLUSTER NODES output and nodes.conf serialization.
     * If NULL, all three are reported as 0. */
    void (*getNodePingPongEpoch)(clusterNode *node, long long *ping_sent, long long *pong_received, uint64_t *config_epoch);

    /* Set protocol-specific per-node fields when loading nodes.conf.
     * ping_sent and pong_received are booleans (non-zero means active).
     * config_epoch is the node's configuration epoch.
     * If NULL, these fields are ignored. */
    void (*setNodePingPongEpoch)(clusterNode *node, int ping_active, int pong_active, uint64_t config_epoch);

    /* Called when a node is marked as failed during nodes.conf loading.
     * If NULL, no protocol-specific action is taken. */
    void (*setNodeFailed)(clusterNode *node);

    /* Append protocol-specific variables to the nodes.conf content.
     * If NULL, no vars line is appended. */
    sds (*appendVarsLine)(sds config);

    /* Parse a protocol-specific variable from the nodes.conf "vars" line.
     * Returns 1 if the variable was handled, 0 otherwise. */
    int (*parseVarsLine)(const char *name, const char *value);

    /* Called after nodes.conf is fully loaded, for post-load fixups
     * (e.g. ensuring currentEpoch >= max configEpoch). If NULL, skipped. */
    void (*postLoad)(void);

    /* Allocate and initialize protocol-specific data for a new node.
     * Sets node->protocol_data. If NULL, protocol_data is left as NULL. */
    void (*initNodeData)(clusterNode *node);

    /* Free protocol-specific per-node data allocated by initNodeData.
     * If NULL, protocol_data is not freed. */
    void (*freeNodeData)(clusterNode *node);

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
    void (*slotChange)(slotRange *ranges, int numranges, clusterNode *target, void *ctx, void (*callback)(void *ctx, const char *error));

    /* Clean up any protocol-specific manual failover state. Called when the
     * node's role changes and any in-progress manual failover state is no
     * longer relevant. */
    void (*resetManualFailoverState)(void);

    /* Reset automatic failover election state. Called when the node switches
     * to a new primary, since any previous election state is stale. */
    void (*resetAutomaticFailoverState)(void);

    /* Node management — called from cluster commands.
     * Each callback performs the protocol-specific action and calls the
     * completion callback when done. The legacy implementation calls it
     * synchronously. A consensus-based implementation may call it after the
     * change is committed. The ctx pointer is passed through to the completion
     * callback. On success, error is NULL. On failure, error points to an error
     * message string. */
    void (*forgetNode)(const char *node_id, size_t id_len, void *ctx, void (*callback)(void *ctx, const char *error));
    void (*setReplicaOf)(clusterNode *primary, void *ctx, void (*callback)(void *ctx, const char *error));
    void (*failover)(int force, int takeover, void *ctx, void (*callback)(void *ctx, const char *error));
    void (*meet)(const char *ip, int port, int cport, void *ctx, void (*callback)(void *ctx, const char *error));
    void (*resetCluster)(int hard);

    /* Handle protocol-specific CLUSTER subcommands (e.g. BUMPEPOCH,
     * SET-CONFIG-EPOCH). Returns 1 if handled, 0 if not recognized. */
    int (*protocolSubcommand)(client *c);
} clusterBusType;

extern clusterBusType *clusterCurrentBus;

#endif /* CLUSTER_BUS_H */
