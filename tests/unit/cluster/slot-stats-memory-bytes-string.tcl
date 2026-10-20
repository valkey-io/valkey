# Integration tests for CLUSTER SLOT-STATS memory-bytes: OBJ_STRING.

source tests/unit/cluster/slot-stats-memory-bytes-helpers.tcl

# -----------------------------------------------------------------------------
# Reply shape.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]

    test "CLUSTER SLOT-STATS reply: memory-bytes is a single per-slot total" {
        set stats [get_slot_stats $key_slot]
        assert {[dict exists $stats memory-bytes]}
        assert_equal 0 [dict get $stats memory-bytes]
    }
}

# -----------------------------------------------------------------------------
# Core string accounting.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]

    # memory-bytes tracks the *allocated* footprint (zmalloc_size) of the value
    # object, which includes the embedded key and rounds up to the allocator's
    # size class. The allocated size is therefore allocator-dependent, but it is
    # always at least the logical size (key bytes + value bytes). These tests
    # assert that lower bound, so they stay portable across allocators.

    test "memory-bytes: SET counts at least key + value bytes" {
        R 0 FLUSHALL
        R 0 SET $key value
        # >= 3 (FOO) + 5 (value) = 8
        assert {[get_slot_bytes $key_slot] >= 8}
    }

    test "memory-bytes: DEL drops the contribution back to 0" {
        R 0 DEL $key
        assert_equal 0 [get_slot_bytes $key_slot]
    }

    test "memory-bytes: SET overwrite replaces old contribution with new" {
        R 0 FLUSHALL
        R 0 SET $key short
        # >= 3 + 5 = 8
        assert {[get_slot_bytes $key_slot] >= 8}

        R 0 SET $key longerVALUE
        # >= 3 + 11 = 14
        assert {[get_slot_bytes $key_slot] >= 14}
    }

    test "memory-bytes: APPEND extends the string in place" {
        R 0 FLUSHALL
        R 0 SET $key value           ;# 3+5 = 8
        R 0 APPEND $key 0123456789   ;# 3+15 = 18
        assert {[get_slot_bytes $key_slot] >= 18}
    }

    test "memory-bytes: SETRANGE on existing key resizes correctly" {
        R 0 FLUSHALL
        R 0 SET $key hello           ;# 3+5 = 8
        R 0 SETRANGE $key 10 world   ;# value len becomes 15: 3+15 = 18
        assert {[get_slot_bytes $key_slot] >= 18}
    }

    test "memory-bytes: SETRANGE on new key creates value at correct size" {
        R 0 FLUSHALL
        R 0 SETRANGE $key 5 abc      ;# value len becomes 8: 3+8 = 11
        assert {[get_slot_bytes $key_slot] >= 11}
    }

    test "memory-bytes: INCR digit-count growth (9 -> 10) is tracked" {
        R 0 FLUSHALL
        R 0 SET $key 9
        # >= 3 + 1 = 4
        assert {[get_slot_bytes $key_slot] >= 4}

        R 0 INCR $key
        # >= 3 + 2 = 5 (value is now "10")
        assert {[get_slot_bytes $key_slot] >= 5}
    }

    test "memory-bytes: DECR digit shrink (10 -> 9) is tracked" {
        R 0 FLUSHALL
        R 0 SET $key 10
        # >= 3 + 2 = 5
        assert {[get_slot_bytes $key_slot] >= 5}

        R 0 DECR $key
        # value is "9": >= 3 + 1 = 4
        assert {[get_slot_bytes $key_slot] >= 4}
    }

    test "memory-bytes: INCRBYFLOAT goes through dbReplaceValue" {
        R 0 FLUSHALL
        R 0 SET $key 1
        # >= 3 + 1 = 4
        assert {[get_slot_bytes $key_slot] >= 4}

        R 0 INCRBYFLOAT $key 0.5
        # value is "1.5": >= 3 + 3 = 6
        assert {[get_slot_bytes $key_slot] >= 6}
    }

    test "memory-bytes: SETBIT extends SDS as needed" {
        R 0 FLUSHALL
        R 0 SETBIT $key 0 1
        # SDS length becomes 1 byte: >= 3+1 = 4
        assert {[get_slot_bytes $key_slot] >= 4}

        R 0 SETBIT $key 23 1
        # SDS length becomes 3 bytes: >= 3+3 = 6
        assert {[get_slot_bytes $key_slot] >= 6}
    }

    test "memory-bytes: type transition string -> list drops string contribution" {
        R 0 FLUSHALL
        R 0 SET $key value
        # Before: at least 8 bytes attributed to string.
        assert {[get_slot_bytes $key_slot] >= 8}

        R 0 DEL $key
        R 0 RPUSH $key element
        # After: lists are not wired up, so the string contribution drops and
        # the slot total returns to 0. The point is that we did not double-count
        # or leak.
        assert_equal 0 [get_slot_bytes $key_slot]
    }

    test "memory-bytes: UNLINK (async delete) subtracts correctly" {
        R 0 FLUSHALL
        R 0 SET $key value
        assert {[get_slot_bytes $key_slot] >= 8}

        R 0 UNLINK $key
        assert_equal 0 [get_slot_bytes $key_slot]
    }

    test "memory-bytes: MSET updates the slot" {
        R 0 FLUSHALL
        # Use hash tags so both keys land in the same slot
        set k1 "{FOO}k1"
        set k2 "{FOO}k2"

        R 0 MSET $k1 hello $k2 world
        # {FOO}k1(7)+hello(5)=12, {FOO}k2(7)+world(5)=12 -> same slot, >= total 24
        assert {[get_slot_bytes $key_slot] >= 24}
    }

    test "memory-bytes: RENAME within same slot updates accounting" {
        R 0 FLUSHALL
        set src "{FOO}src"
        set dst "{FOO}dst"

        R 0 SET $src value
        # {FOO}src(8) + value(5) = 13
        assert {[get_slot_bytes $key_slot] >= 13}

        R 0 RENAME $src $dst
        # src gone, dst has: {FOO}dst(8) + value(5) = 13
        assert {[get_slot_bytes $key_slot] >= 13}
    }

    test "memory-bytes: key expiration subtracts correctly" {
        R 0 FLUSHALL
        R 0 SET $key value PX 50
        assert {[get_slot_bytes $key_slot] >= 8}

        after 100
        R 0 GET $key
        assert_equal 0 [get_slot_bytes $key_slot]
    }
}

# -----------------------------------------------------------------------------
# Bulk wipe semantics.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]

    test "memory-bytes: FLUSHDB clears the state metric" {
        R 0 FLUSHALL
        R 0 SET $key value
        assert {[get_slot_bytes $key_slot] >= 8}

        R 0 FLUSHDB
        assert_equal 0 [get_slot_bytes $key_slot]
    }

    test "memory-bytes: FLUSHALL clears the state metric" {
        R 0 SET $key value
        assert {[get_slot_bytes $key_slot] >= 8}

        R 0 FLUSHALL
        assert_equal 0 [get_slot_bytes $key_slot]
    }
}

# -----------------------------------------------------------------------------
# CONFIG RESETSTAT preserves memory-bytes (state, not cumulative).
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster} overrides {cluster-slot-stats-enabled yes}} {
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]

    test "memory-bytes: CONFIG RESETSTAT preserves memory-bytes state metric" {
        R 0 FLUSHALL
        R 0 SET $key value
        set before [get_slot_bytes $key_slot]
        assert {$before >= 8}

        R 0 CONFIG RESETSTAT

        # memory-bytes still reflects current key memory usage (unchanged).
        assert_equal $before [get_slot_bytes $key_slot]
        # cpu-usec / network-bytes-* should have been reset.
        set stats [get_slot_stats $key_slot]
        assert_equal 0 [dict get $stats cpu-usec]
        assert_equal 0 [dict get $stats network-bytes-in]
    }
}

# -----------------------------------------------------------------------------
# ORDERBY memory-bytes.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {
    test "memory-bytes: ORDERBY memory-bytes DESC sorts by total per-slot bytes" {
        R 0 FLUSHALL
        R 0 SET key:small a
        R 0 SET key:large [string repeat z 200]

        set out [R 0 CLUSTER SLOT-STATS ORDERBY memory-bytes LIMIT 2 DESC]
        set first_total  [dict get [lindex [lindex $out 0] 1] memory-bytes]
        set second_total [dict get [lindex [lindex $out 1] 1] memory-bytes]
        assert {$first_total >= $second_total}
        assert {$first_total > 0}
    }

    test "memory-bytes: ORDERBY memory-bytes is allowed without cluster-slot-stats-enabled" {
        R 0 CONFIG SET cluster-slot-stats-enabled no
        R 0 FLUSHALL
        R 0 SET FOO value
        set out [R 0 CLUSTER SLOT-STATS ORDERBY memory-bytes LIMIT 1 DESC]
        assert_equal 1 [llength $out]
        R 0 CONFIG SET cluster-slot-stats-enabled yes
    }
}

# -----------------------------------------------------------------------------
# Exact accuracy: memory-bytes must equal the sum of MEMORY USAGE (SAMPLES 0)
# over the keys in the slot. This is the authoritative footprint, so the match
# is byte-for-byte rather than a bound.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]

    test "memory-bytes matches MEMORY USAGE: EMBSTR value" {
        R 0 FLUSHALL
        R 0 SET $key hi
        assert_slot_bytes_match_memory_usage $key_slot
    }

    test "memory-bytes matches MEMORY USAGE: INT-encoded value" {
        R 0 FLUSHALL
        R 0 SET $key 12345
        assert_slot_bytes_match_memory_usage $key_slot
    }

    test "memory-bytes matches MEMORY USAGE: RAW value" {
        R 0 FLUSHALL
        R 0 SET $key [string repeat x 100]
        assert_slot_bytes_match_memory_usage $key_slot
    }

    test "memory-bytes matches MEMORY USAGE: after APPEND grows the value" {
        R 0 FLUSHALL
        R 0 SET $key value
        R 0 APPEND $key [string repeat y 200]
        assert_slot_bytes_match_memory_usage $key_slot
    }

    test "memory-bytes matches MEMORY USAGE: multiple keys in one slot" {
        R 0 FLUSHALL
        R 0 MSET "{FOO}k1" hello "{FOO}k2" [string repeat z 50] "{FOO}k3" 999
        assert_slot_bytes_match_memory_usage $key_slot
    }

    # Adding an expire can reallocate the value to hold the embedded expire
    # field. setExpire must account that change.
    test "memory-bytes matches MEMORY USAGE: SET EX / SETEX / SET PX" {
        R 0 FLUSHALL
        R 0 SET "{FOO}k1" [string repeat a 20] EX 100
        R 0 SETEX "{FOO}k2" 100 [string repeat b 30]
        R 0 SET "{FOO}k3" 12345 PX 100000
        R 0 SET "{FOO}k4" [string repeat c 100] EX 100
        assert_slot_bytes_match_memory_usage $key_slot
    }

    test "memory-bytes matches MEMORY USAGE: EXPIRE / GETEX on an existing key" {
        R 0 FLUSHALL
        R 0 SET "{FOO}k1" [string repeat a 20]
        R 0 SET "{FOO}k2" [string repeat b 30]
        R 0 EXPIRE "{FOO}k1" 100
        R 0 GETEX "{FOO}k2" EX 100
        assert_slot_bytes_match_memory_usage $key_slot
        R 0 PERSIST "{FOO}k1"
        assert_slot_bytes_match_memory_usage $key_slot
    }

    test "memory-bytes matches MEMORY USAGE: volatile keys after DEBUG RELOAD" {
        R 0 FLUSHALL
        R 0 SET "{FOO}k1" [string repeat a 20] EX 100
        R 0 SET "{FOO}k2" [string repeat b 100]
        R 0 DEBUG RELOAD
        assert_slot_bytes_match_memory_usage $key_slot
        R 0 DEL "{FOO}k1" "{FOO}k2"
        assert_equal 0 [get_slot_bytes $key_slot]
    } {} {needs:debug}

    # Commands run from a script execute on a temporary client, not on the
    # client that runs EVAL. The snapshot and the commit must use the same client.
    test "memory-bytes matches MEMORY USAGE: in-place APPEND / SETRANGE inside EVAL" {
        R 0 FLUSHALL
        R 0 SET $key value
        R 0 EVAL {return server.call('APPEND', KEYS[1], ARGV[1])} 1 $key [string repeat x 100]
        assert_slot_bytes_match_memory_usage $key_slot
        R 0 EVAL {return server.call('SETRANGE', KEYS[1], 300, 'y')} 1 $key
        assert_slot_bytes_match_memory_usage $key_slot
    }

    # MSETNX looks up every key before it sets any of them, so the snapshot set
    # holds one entry per key and uses the overflow hashtable.
    test "memory-bytes matches MEMORY USAGE: MSETNX with many keys" {
        R 0 FLUSHALL
        set args {}
        for {set i 0} {$i < 1000} {incr i} {
            lappend args "{FOO}n$i" [string repeat v [expr {$i % 200}]]
        }
        assert_equal 1 [R 0 MSETNX {*}$args]
        assert_slot_bytes_match_memory_usage $key_slot
    }

    # A write lookup that changes nothing (SET NX on an existing key) leaves a
    # snapshot that is dropped when that sub-command ends. A later sub-command
    # that changes the same key in place must still be counted.
    test "memory-bytes: no-op write then in-place change in one EVAL / MULTI" {
        R 0 FLUSHALL
        R 0 SET "{FOO}other" [string repeat b 100]
        R 0 SET $key [string repeat a 1000]
        R 0 EVAL {
            server.call('SET', KEYS[1], 'x', 'NX')
            server.call('HDEL', KEYS[2], 'f')
            return server.call('APPEND', KEYS[1], ARGV[1])
        } 2 $key "{FOO}missing" [string repeat x 500]
        assert_slot_bytes_match_memory_usage $key_slot

        R 0 MULTI
        R 0 SET $key x NX
        R 0 APPEND $key [string repeat y 700]
        R 0 EXEC
        assert_slot_bytes_match_memory_usage $key_slot
    }
}
