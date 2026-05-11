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

/**
 * Register the built-in durability providers. Called from durabilityInit().
 */
void registerBuiltinDurabilityProviders(void) {
    /* Only register if not already registered (idempotent) */
    if (num_durability_providers == 0) {
        registerDurabilityProvider(&builtinAofProvider);
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
