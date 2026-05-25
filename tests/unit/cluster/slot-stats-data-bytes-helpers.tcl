# Shared constants and helpers for CLUSTER SLOT-STATS data-bytes tests.
# Source this file from per-type test files via:
#   source tests/unit/cluster/slot-stats-data-bytes-helpers.tcl

# -----------------------------------------------------------------------------
# Object-type name constants. Mirror the OBJ_* macros in src/server.h. The
# values are the exact strings used by the CLUSTER SLOT-STATS reply as keys
# inside the data-bytes nested map (see slotStatsObjectTypeName() in
# src/cluster_slot_stats.c).
# -----------------------------------------------------------------------------

set ::OBJ_STRING "string"
set ::OBJ_LIST   "list"
set ::OBJ_SET    "set"
set ::OBJ_ZSET   "zset"
set ::OBJ_HASH   "hash"
set ::OBJ_MODULE "module"
set ::OBJ_STREAM "stream"

# All object-type names in the same order as OBJ_STRING ... OBJ_STREAM in
# src/server.h. Useful for asserting the full data-bytes map shape.
set ::OBJ_TYPE_NAMES [list \
    $::OBJ_STRING $::OBJ_LIST $::OBJ_SET $::OBJ_ZSET \
    $::OBJ_HASH $::OBJ_MODULE $::OBJ_STREAM]

# -----------------------------------------------------------------------------
# Helpers.
#
# All helpers default to node 0 (the single primary spun up by start_cluster
# in this file). Pass an explicit node index as the last argument when
# multi-node tests are added.
# -----------------------------------------------------------------------------

# Returns the full per-slot stats map for `slot`, e.g.
#   {key-count 1 data-bytes {string 8 list 0 ...} cpu-usec 0 ...}
proc get_slot_stats {slot {n 0}} {
    set slot_stats [R $n CLUSTER SLOT-STATS SLOTSRANGE $slot $slot]
    set entry [lindex $slot_stats 0]
    return [lindex $entry 1]
}

# Returns the data-bytes value for a given (type, slot) pair.
proc get_slot_bytes {type slot {n 0}} {
    set stats [get_slot_stats $slot $n]
    return [dict get $stats data-bytes $type]
}

# Returns the full per-type data-bytes map for `slot`.
proc get_slot_bytes_map {slot {n 0}} {
    set stats [get_slot_stats $slot $n]
    return [dict get $stats data-bytes]
}
