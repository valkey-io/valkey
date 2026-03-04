#ifndef DURABILITY_PROVIDER_H
#define DURABILITY_PROVIDER_H

#include <stdbool.h>

/*================================= Durability Provider Interface ============ */

/**
 * Maximum number of durability providers that can be registered.
 * Built-in providers: replica, aof.
 */
#define MAX_DURABILITY_PROVIDERS 4

/**
 * A durability provider represents a source of durability acknowledgment.
 * Each provider tracks progress independently and the overall durability
 * consensus is the MIN (AND) of all enabled providers' acknowledged offsets.
 *
 * Examples: replica acknowledgments, AOF fsync.
 */
typedef struct durabilityProvider {
    const char *name;                  /* Human-readable name, e.g. "replica", "aof" */
    bool (*isEnabled)(void);           /* Is this provider currently active? */
    long long (*getAckedOffset)(void); /* What offset has this provider acknowledged? */
} durabilityProvider;

/* Provider registry */
void registerDurabilityProvider(durabilityProvider *provider);
void unregisterDurabilityProvider(durabilityProvider *provider);
bool anyDurabilityProviderEnabled(void);

/**
 * Returns the durability consensus offset by iterating all registered
 * providers and returning the MIN of all enabled providers' acknowledged
 * offsets (AND semantics: all must acknowledge).
 */
long long getDurabilityConsensusOffset(void);

/**
 * Register the built-in durability providers (replica + AOF).
 * Called from syncReplicationInit().
 */
void registerBuiltinDurabilityProviders(void);

/**
 * Reset the durability provider registry (for cleanup/shutdown).
 */
void resetDurabilityProviders(void);

#endif /* DURABILITY_PROVIDER_H */
