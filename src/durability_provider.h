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
    bool paused;                       /* When true (via DEBUG), getAckedOffset() returns
                                        * the offset captured at pause time to freeze
                                        * consensus progress. Used for testing. */
    long long pausedOffset;            /* Offset snapshot taken when provider is paused. */
} durabilityProvider;

/* Provider registry */
void registerDurabilityProvider(durabilityProvider *provider);
void unregisterDurabilityProvider(durabilityProvider *provider);
bool anyDurabilityProviderEnabled(void);
bool pauseDurabilityProvider(const char *name);
bool resumeDurabilityProvider(const char *name);

/**
 * Returns the durability consensus offset by iterating all registered
 * providers and returning the MIN of all enabled providers' acknowledged
 * offsets (AND semantics: all must acknowledge).
 */
long long getDurabilityConsensusOffset(void);

/**
 * Register the built-in durability providers (replica + AOF).
 * Called from durabilityInit().
 */
void registerBuiltinDurabilityProviders(void);

/**
 * Reset the durability provider registry (for cleanup/shutdown).
 */
void resetDurabilityProviders(void);

#endif /* DURABILITY_PROVIDER_H */
