#ifndef TRACKING_H
#define TRACKING_H

#include "deps/jemalloc/include/jemalloc/internal/arena_structs.h"

struct tracking_table {
    struct arena *arena;
};

void tracking_table_defragment(struct tracking_table *table);
void tracking_table_sweep_dead_clients(struct tracking_table *table);
void tracking_table_periodic_sweep(struct tracking_table *table);

#endif /* TRACKING_H */