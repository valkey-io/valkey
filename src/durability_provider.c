#include "server.h"
#include <assert.h>
#include <stdatomic.h>

/*================================= Durability Provider Registry ============= */

/* Provider registry: static array of registered providers */
static durabilityProvider *durability_providers[MAX_DURABILITY_PROVIDERS];
static int num_durability_providers = 0;

/**
 * Register a durability provider. Providers are checked in registration order.
 * The overall durability consensus is the MIN (AND) of all enabled providers.
 */
void registerDurabilityProvider(durabilityProvider *provider) {
    serverAssert(num_durability_providers < MAX_DURABILITY_PROVIDERS);
    durability_providers[num_durability_providers++] = provider;
    serverLog(LL_NOTICE, "Registered durability provider: %s", provider->name);
}

/**
 * Unregister a durability provider by pointer.
 */
void unregisterDurabilityProvider(durabilityProvider *provider) {
    for (int i = 0; i < num_durability_providers; i++) {
        if (durability_providers[i] == provider) {
            /* Shift remaining providers down */
            for (int j = i; j < num_durability_providers - 1; j++) {
                durability_providers[j] = durability_providers[j + 1];
            }
            num_durability_providers--;
            serverLog(LL_NOTICE, "Unregistered durability provider: %s", provider->name);
            return;
        }
    }
}

bool anyDurabilityProviderEnabled(void) {
    for (int i = 0; i < num_durability_providers; i++) {
        if (durability_providers[i]->isEnabled()) return true;
    }
    return false;
}

/**
 * Reset the durability provider registry so it can be re-initialized.
 */
void resetDurabilityProviders(void) {
    num_durability_providers = 0;
}

/*================================= Built-in AOF Provider ==================== */

static bool aofProviderIsEnabled(void) {
    return server.aof_state != AOF_OFF && server.aof_fsync == AOF_FSYNC_ALWAYS;
}

static long long aofProviderGetAckedOffset(void) {
    /* Use fsynced_reploff_pending directly instead of fsynced_reploff.
     * When async AOF flushing is used (IO threads), fsynced_reploff_pending
     * is updated by the IO thread upon fsync completion, but fsynced_reploff
     * is only updated in the next beforeSleep() iteration. Using the pending
     * value ensures we see the most up-to-date fsync progress immediately. */
    long long fsynced_offset = atomic_load_explicit(&server.fsynced_reploff_pending, memory_order_relaxed);
    /* Handle the case where AOF is enabled but no data has been fsynced yet
     * (fsynced_reploff_pending is 0 initially). In that case, use fsynced_reploff
     * if it's been properly initialized. */
    if (fsynced_offset == 0 && server.fsynced_reploff > 0) {
        fsynced_offset = server.fsynced_reploff;
    }
    return fsynced_offset;
}

static durabilityProvider builtinAofProvider = {
    .name = "aof",
    .isEnabled = aofProviderIsEnabled,
    .getAckedOffset = aofProviderGetAckedOffset,
    .paused = false,
    .pausedOffset = 0,
};

/*================================= Built-in Replication Provider ============ */

/**
 * The replication durability provider is enabled when min-sync-replicas > 0.
 * This implements the sync replication data path from the PacificA framework:
 * writes are only considered committed once acknowledged by at least
 * min-sync-replicas sync replicas (replicas with REPLICA_CAPA_SYNC flag).
 */
static bool replicationProviderIsEnabled(void) {
    return server.sync_replication_enabled == 1;
}

/**
 * Compute the consensus offset across all sync replicas.
 *
 * For every REPLCONF ACK, we calculate the minimum ack offset of all
 * online sync replicas (those in the ISR — with is_in_sync flag set).
 *
 * consensus_offset = minimum_ack_offset(list of sync replicas)
 *
 * A replica is in the ISR when:
 *   1. It declared REPLICA_CAPA_SYNC capability via REPLCONF
 *   2. Its repl_ack_off caught up to the committed_offset
 *   3. It has not timed out (checked by replicationCron)
 *
 * If there are fewer ISR members than min-sync-replicas,
 * returns -1 to block consensus advancement (the shard is not writable).
 */
static long long replicationProviderGetAckedOffset(void) {
    listIter li;
    listNode *ln;
    int sync_replica_count = 0;
    long long min_offset = LLONG_MAX;

    listRewind(server.replicas, &li);
    while ((ln = listNext(&li))) {
        client *replica = ln->value;

        /* Only consider replicas that are online and in the ISR. */
        if (replica->repl_data->repl_state != REPLICA_STATE_ONLINE) continue;
        if (!replica->repl_data->is_in_sync) continue;

        sync_replica_count++;
        if (replica->repl_data->repl_ack_off < min_offset) {
            min_offset = replica->repl_data->repl_ack_off;
        }
    }

    /* If we don't have enough sync replicas, block consensus. */
    if (sync_replica_count < server.min_sync_replicas) {
        return -1;
    }

    /* If min_offset was never updated (shouldn't happen given count check),
     * return 0 as a safe fallback. */
    return (min_offset == LLONG_MAX) ? 0 : min_offset;
}

static durabilityProvider builtinReplicationProvider = {
    .name = "replication",
    .isEnabled = replicationProviderIsEnabled,
    .getAckedOffset = replicationProviderGetAckedOffset,
    .paused = false,
    .pausedOffset = 0,
};

/**
 * Register the built-in durability providers. Called from durabilityInit().
 *
 * Currently the AOF provider and replication provider are built-in.
 */
void registerBuiltinDurabilityProviders(void) {
    /* Only register if not already registered (idempotent) */
    if (num_durability_providers == 0) {
        registerDurabilityProvider(&builtinAofProvider);
        registerDurabilityProvider(&builtinReplicationProvider);
    }
}

/*================================= Consensus Calculation ==================== */

/**
 * Returns the durability consensus offset by iterating all registered
 * providers and returning the MIN of all enabled providers' acknowledged
 * offsets (AND semantics: all must acknowledge).
 *
 * If a provider returns -1, it means the provider cannot make progress
 * (e.g. insufficient replicas), which blocks consensus advancement.
 *
 * If no providers are enabled, returns server.primary_repl_offset
 * (i.e. no blocking).
 */
long long getDurabilityConsensusOffset(void) {
    long long consensus = server.primary_repl_offset;
    bool any_enabled = false;

    for (int i = 0; i < num_durability_providers; i++) {
        durabilityProvider *p = durability_providers[i];
        if (!p->isEnabled()) continue;
        any_enabled = true;

        long long offset;
        if (p->paused) {
            /* Paused provider (via DEBUG) returns the offset snapshot
             * captured at pause time, freezing consensus at that point. */
            offset = p->pausedOffset;
        } else {
            offset = p->getAckedOffset();
        }

        if (offset == -1) {
            /* Provider cannot make progress — block consensus. */
            return -1;
        }
        if (offset < consensus) consensus = offset;
    }

    return any_enabled ? consensus : server.primary_repl_offset;
}

/**
 * Pause a durability provider by name (via DEBUG command).
 * When paused, the provider's current acknowledged offset is captured and
 * frozen — any writes after the pause point will block until the provider
 * is resumed and catches up.
 * Returns true if provider was found, false otherwise.
 */
bool pauseDurabilityProvider(const char *name) {
    for (int i = 0; i < num_durability_providers; i++) {
        if (!strcasecmp(durability_providers[i]->name, name)) {
            /* Snapshot the current acked offset before pausing so that
             * writes already acknowledged remain unblocked. */
            durability_providers[i]->pausedOffset = durability_providers[i]->getAckedOffset();
            durability_providers[i]->paused = true;
            serverLog(LL_NOTICE, "Paused durability provider: %s (frozen at offset %lld)",
                      name, durability_providers[i]->pausedOffset);
            return true;
        }
    }
    return false;
}

/**
 * Resume a durability provider by name (via DEBUG command).
 * After resuming, triggers a durability progress check to unblock
 * any clients that can now proceed.
 * Returns true if provider was found, false otherwise.
 */
bool resumeDurabilityProvider(const char *name) {
    for (int i = 0; i < num_durability_providers; i++) {
        if (!strcasecmp(durability_providers[i]->name, name)) {
            durability_providers[i]->paused = false;
            /* Trigger a durability check to unblock any clients that can now proceed */
            notifyDurabilityProgress();
            serverLog(LL_NOTICE, "Resumed durability provider: %s", name);
            return true;
        }
    }
    return false;
}
