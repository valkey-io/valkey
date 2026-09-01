/*
 * CLEAN_STATE_FOR_DOLLY_SAVE
 *
 * Admin-only command executed on a CRIU-cloned valkey-server process before
 * it rejoins a cluster as a replica of the primary it was cloned from.
 *
 * Use case: a replica in a shard fails, and instead of provisioning a fresh
 * replica (which would require the primary to run a full BGSAVE + RDB
 * transfer - hours on a multi-hundred-GB dataset), we CRIU-clone the primary
 * to a new host. The clone already holds every byte of the primary's data in
 * memory; all we need is to point it at the primary and have it PSYNC at its
 * current replication offset. This command fixes the clone's identity state
 * so that PSYNC - not a full resync - will succeed.
 *
 * Runtime identity (runid, pid, starttime, availability_zone) is regenerated
 * so the clone is observably a distinct process.
 *
 * Replication identity is PRESERVED and additionally a cached_primary is
 * synthesized from server.replid + server.primary_repl_offset so that the
 * subsequent REPLICAOF / CLUSTER REPLICATE emits "PSYNC <replid> <offset+1>"
 * and lands a +CONTINUE against the source primary.
 *
 * Cluster identity is regenerated (new node id + shard id, peers forgotten,
 * epochs zeroed, announce addresses cleared). Slot ownership on `myself` is
 * cleared so the clone does not advertise the source shard's slots. The
 * cluster-level CLUSTER_NODE_REPLICA / PRIMARY flag on myself is left alone:
 * the subsequent CLUSTER REPLICATE will set it correctly, and leaving the
 * clone as a cluster-level replica (with replicaof=NULL) sidesteps the
 * "primary with non-empty keyspace" preflight in CLUSTER REPLICATE.
 *
 * What this command does NOT touch:
 *   - The keyspace, AOF, RDB state, config, ACLs, scripts.
 *   - server.replid / primary_repl_offset / replication backlog.
 *   - Open client connections. CRIU --tcp-close leaves dead FDs that are
 *     naturally reaped by normal TCP error / timeout paths.
 *   - Listener FDs, bind addresses, ports: config-driven, rebound by CRIU
 *     restore per the destination valkey.conf.
 */

#include "server.h"
#include "cluster.h"
#include "cluster_legacy.h"
#include "connection.h"

#include <unistd.h>
#include <string.h>

/* Forward declarations for helpers that are defined in cluster_legacy.c and
 * replication.c but are not exposed via public headers. */
void replicationDiscardCachedPrimary(void);
void replicationCachePrimaryUsingMyself(void);
void clusterAddNode(clusterNode *node);
void clusterDelNode(clusterNode *delnode);
int clusterDelNodeSlots(clusterNode *node);
void clusterAddNodeToShard(const char *shard_id, clusterNode *node);
void resetManualFailover(void);
void clusterCloseAllSlots(void);

#define DOLLY_LOG_PREFIX "CLEAN_STATE_FOR_DOLLY_SAVE:"

static void dollyResetRuntimeIdentity(void) {
    serverLog(LL_WARNING, DOLLY_LOG_PREFIX " regenerating runtime identity (runid, pid, starttime)");
    getRandomHexChars(server.runid, CONFIG_RUN_ID_SIZE);
    server.runid[CONFIG_RUN_ID_SIZE] = '\0';
    server.pid = getpid();
    server.stat_starttime = time(NULL);

    /* Clear availability zone (cloud infrastructure metadata that identifies
     * the source machine's location). Set to empty string, not NULL, because
     * INFO uses it directly without a NULL check. */
    if (server.availability_zone) {
        sdsfree(server.availability_zone);
        server.availability_zone = sdsempty();
    }
}

static void dollyResetReplicationIdentity(void) {
    serverLog(LL_WARNING, DOLLY_LOG_PREFIX " clearing in-flight replication transfer state");

    /* Close in-flight RDB-transfer sockets and temp files. Their FDs will be
     * dead after CRIU restore and the tmp file path belongs to the source
     * machine's filesystem namespace. */
    if (server.repl_transfer_s != NULL) {
        connClose(server.repl_transfer_s);
        server.repl_transfer_s = NULL;
    }
    if (server.repl_rdb_transfer_s != NULL) {
        connClose(server.repl_rdb_transfer_s);
        server.repl_rdb_transfer_s = NULL;
    }
    if (server.repl_transfer_fd != -1) {
        close(server.repl_transfer_fd);
        server.repl_transfer_fd = -1;
    }
    if (server.repl_transfer_tmpfile != NULL) {
        unlink(server.repl_transfer_tmpfile);
        zfree(server.repl_transfer_tmpfile);
        server.repl_transfer_tmpfile = NULL;
    }
    server.repl_rdb_channel_state = REPL_DUAL_CHANNEL_STATE_NONE;

    if (server.repl_provisional_primary.conn != NULL) {
        connClose(server.repl_provisional_primary.conn);
        server.repl_provisional_primary.conn = NULL;
    }

    /* Synthesize a cached_primary from our own replid + offset so that the
     * upcoming REPLICAOF / CLUSTER REPLICATE emits "PSYNC <replid> <offset+1>"
     * instead of "PSYNC ? -1", and the upstream can answer +CONTINUE.
     *
     * Only do this when the clone is of a primary (server.primary_host == NULL)
     * and we don't already have a cached_primary. If the clone was of a replica
     * that already had a cached_primary, that one is what PSYNC should use. */
    if (server.primary_host == NULL && server.cached_primary == NULL) {
        serverLog(LL_WARNING,
                  DOLLY_LOG_PREFIX " synthesizing cached_primary for PSYNC (replid=%.40s offset=%lld)",
                  server.replid, server.primary_repl_offset);
        replicationCachePrimaryUsingMyself();
    }
}

static void dollyResetClusterIdentity(void) {
    if (!server.cluster_enabled || server.cluster == NULL || server.cluster->myself == NULL) {
        serverLog(LL_WARNING, DOLLY_LOG_PREFIX " cluster mode disabled, skipping cluster identity reset");
        return;
    }

    clusterNode *me = server.cluster->myself;

    serverLog(LL_WARNING, DOLLY_LOG_PREFIX " regenerating cluster node ID and forgetting peers");

    /* Regenerate node ID + shard_id, re-key self in the nodes dict.
     * Same pattern as clusterReset() in cluster_legacy.c. */
    sds oldname = sdsnewlen(me->name, CLUSTER_NAMELEN);
    dictDelete(server.cluster->nodes, oldname);
    sdsfree(oldname);
    getRandomHexChars(me->name, CLUSTER_NAMELEN);
    getRandomHexChars(me->shard_id, CLUSTER_NAMELEN);
    clusterAddNode(me);

    /* Forget all non-self nodes. */
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node == me) continue;
        clusterDelNode(node);
    }
    dictReleaseIterator(di);

    dictEmpty(server.cluster->nodes_black_list, NULL);

    /* Rebuild the shards dict containing only self under the new shard_id. */
    dictEmpty(server.cluster->shards, NULL);
    clusterAddNodeToShard(me->shard_id, me);

    /* Clear myself's slot bitmap. The CRIU clone of a primary inherited its
     * slot claim; we must release it because:
     *   1. clusterSetPrimary (invoked by CLUSTER REPLICATE) asserts
     *      myself->numslots == 0.
     *   2. While the clone is transitioning to a replica of the source shard,
     *      it should not advertise slot ownership to the destination cluster. */
    if (me->numslots > 0) {
        serverLog(LL_WARNING, DOLLY_LOG_PREFIX " clearing %d slot claim(s) on myself", me->numslots);
        clusterDelNodeSlots(me);
    }

    /* Flip myself to CLUSTER_NODE_REPLICA with no primary. This makes the
     * subsequent CLUSTER REPLICATE preflight (src/cluster_legacy.c:8084)
     * skip its "primary must be empty" guard - the clone's keyspace is
     * intentionally non-empty, and as a replica we're allowed to switch
     * primaries freely. clusterSetPrimary will overwrite replicaof and
     * leave the REPLICA flag in place.
     *
     * We deliberately do NOT call clusterSetNodeAsPrimary -> that would
     * invoke replicationUnsetPrimary which frees server.primary, discards
     * server.cached_primary, and shifts server.replid - destroying the
     * PSYNC state we just set up. */
    me->flags &= ~CLUSTER_NODE_PRIMARY;
    me->flags |= CLUSTER_NODE_REPLICA;
    me->replicaof = NULL;

    /* Reset cluster epochs. */
    server.cluster->currentEpoch = 0;
    server.cluster->lastVoteEpoch = 0;
    me->configEpoch = 0;

    /* Clear in-flight failover state. */
    resetManualFailover();
    server.cluster->failover_auth_time = 0;
    server.cluster->failover_auth_count = 0;
    server.cluster->failover_auth_sent = 0;
    server.cluster->failover_auth_rank = 0;
    server.cluster->failover_auth_epoch = 0;
    server.cluster->failover_failed_primary_rank = 0;
    server.cluster->cant_failover_reason = CLUSTER_CANT_FAILOVER_NONE;

    /* Drop in-flight slot migration state (legacy migrating/importing dicts). */
    clusterCloseAllSlots();

    /* Clear cached announce strings owned by config (char* allocated via zstrdup).
     * Configs are created with EMPTY_STRING_IS_NULL, so NULL is a valid state. */
    serverLog(LL_WARNING, DOLLY_LOG_PREFIX " clearing cached cluster announce addresses");
    if (server.cluster_announce_ip) {
        zfree(server.cluster_announce_ip);
        server.cluster_announce_ip = NULL;
    }
    if (server.cluster_announce_client_ipv4) {
        zfree(server.cluster_announce_client_ipv4);
        server.cluster_announce_client_ipv4 = NULL;
    }
    if (server.cluster_announce_client_ipv6) {
        zfree(server.cluster_announce_client_ipv6);
        server.cluster_announce_client_ipv6 = NULL;
    }
    if (server.cluster_announce_hostname) {
        zfree(server.cluster_announce_hostname);
        server.cluster_announce_hostname = NULL;
    }
    if (server.cluster_announce_human_nodename) {
        zfree(server.cluster_announce_human_nodename);
        server.cluster_announce_human_nodename = NULL;
    }

    /* Clear cached announce strings on myself (sds allocations). */
    me->ip[0] = '\0';
    sdsfree(me->hostname);
    me->hostname = sdsempty();
    sdsfree(me->human_nodename);
    me->human_nodename = sdsempty();
    sdsfree(me->announce_client_ipv4);
    me->announce_client_ipv4 = sdsempty();
    sdsfree(me->announce_client_ipv6);
    me->announce_client_ipv6 = sdsempty();

    /* Clear cached CLUSTER SLOTS responses (contain old node IDs/IPs). */
    for (int i = 0; i < CACHE_CONN_TYPE_MAX; i++) {
        if (server.cached_cluster_slot_info[i]) {
            sdsfree(server.cached_cluster_slot_info[i]);
            server.cached_cluster_slot_info[i] = NULL;
        }
    }

    /* Persist the new identity to nodes.conf and recompute cluster state. */
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_FSYNC_CONFIG);

    serverLog(LL_WARNING, DOLLY_LOG_PREFIX " cluster reset complete, new node ID %.40s", me->name);
}

void cleanStateForDollySaveCommand(client *c) {
    serverLog(LL_WARNING, DOLLY_LOG_PREFIX " starting clean-state for CRIU migration");

    dollyResetRuntimeIdentity();
    dollyResetReplicationIdentity();
    dollyResetClusterIdentity();

    serverLog(LL_WARNING, DOLLY_LOG_PREFIX " complete");
    addReply(c, shared.ok);
}
