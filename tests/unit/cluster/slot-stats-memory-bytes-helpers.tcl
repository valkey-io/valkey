# Shared helpers for CLUSTER SLOT-STATS memory-bytes tests.
# Source this file from per-type test files via:
#   source tests/unit/cluster/slot-stats-memory-bytes-helpers.tcl

# -----------------------------------------------------------------------------
# Helpers.
#
# All helpers default to node 0 (the single primary spun up by start_cluster
# in this file). Pass an explicit node index as the last argument when
# multi-node tests are added.
# -----------------------------------------------------------------------------

# Returns the full per-slot stats map for `slot`, e.g.
#   {key-count 1 memory-bytes 8 cpu-usec 0 ...}
proc get_slot_stats {slot {n 0}} {
    set slot_stats [R $n CLUSTER SLOT-STATS SLOTSRANGE $slot $slot]
    set entry [lindex $slot_stats 0]
    return [lindex $entry 1]
}

# Returns the memory-bytes total for `slot`. memory-bytes is a single per-slot
# counter summed across all keys in the slot, regardless of object type.
proc get_slot_bytes {slot {n 0}} {
    set stats [get_slot_stats $slot $n]
    return [dict get $stats memory-bytes]
}

# Returns the sum of `MEMORY USAGE <key> SAMPLES 0` over every key in `slot`.
# SAMPLES 0 forces the full (non-sampled) walk, so the result is the exact
# allocated footprint and is directly comparable to the slot's memory-bytes.
#
# This equivalence holds for the object types currently wired into the
# memory-bytes metric (strings, and streams without consumer groups). It does
# NOT hold once a stream has consumer groups, since MEMORY USAGE counts the
# consumer-group memory while memory-bytes does not (yet).
proc get_slot_memory_usage {slot {n 0}} {
    set total 0
    foreach key [R $n CLUSTER GETKEYSINSLOT $slot 1000000] {
        incr total [R $n MEMORY USAGE $key SAMPLES 0]
    }
    return $total
}

# Asserts that the slot's memory-bytes counter exactly equals the sum of
# MEMORY USAGE over the keys in the slot. See get_slot_memory_usage for the
# scope of this equivalence.
proc assert_slot_bytes_match_memory_usage {slot {n 0}} {
    assert_equal [get_slot_memory_usage $slot $n] [get_slot_bytes $slot $n]
}
