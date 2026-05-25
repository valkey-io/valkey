# Integration tests for CLUSTER SLOT-STATS data-bytes: OBJ_STRING.

source tests/unit/cluster/slot-stats-data-bytes-helpers.tcl

# -----------------------------------------------------------------------------
# Reply shape.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]

    test "CLUSTER SLOT-STATS reply: data-bytes is a 7-entry map with valid type names" {
        set db [get_slot_bytes_map $key_slot]
        assert_equal 7 [dict size $db]
        foreach t $::OBJ_TYPE_NAMES {
            assert {[dict exists $db $t]}
            assert_equal 0 [dict get $db $t]
        }
    }
}

# -----------------------------------------------------------------------------
# Core string accounting.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]

    test "data-bytes: SET counts key + value bytes as string" {
        R 0 FLUSHALL
        R 0 SET $key value
        # 3 (FOO) + 5 (value) = 8
        assert_equal 8 [get_slot_bytes $::OBJ_STRING $key_slot]
    }

    test "data-bytes: DEL drops the contribution back to 0" {
        R 0 DEL $key
        assert_equal 0 [get_slot_bytes $::OBJ_STRING $key_slot]
    }

    test "data-bytes: SET overwrite replaces old contribution with new" {
        R 0 FLUSHALL
        R 0 SET $key short
        # 3 + 5 = 8
        assert_equal 8 [get_slot_bytes $::OBJ_STRING $key_slot]

        R 0 SET $key longerVALUE
        # 3 + 11 = 14
        assert_equal 14 [get_slot_bytes $::OBJ_STRING $key_slot]
    }

    test "data-bytes: APPEND extends the string in place" {
        R 0 FLUSHALL
        R 0 SET $key value           ;# 3+5 = 8
        R 0 APPEND $key 0123456789   ;# 3+15 = 18
        assert_equal 18 [get_slot_bytes $::OBJ_STRING $key_slot]
    }

    test "data-bytes: SETRANGE on existing key resizes correctly" {
        R 0 FLUSHALL
        R 0 SET $key hello           ;# 3+5 = 8
        R 0 SETRANGE $key 10 world   ;# value len becomes 15: 3+15 = 18
        assert_equal 18 [get_slot_bytes $::OBJ_STRING $key_slot]
    }

    test "data-bytes: SETRANGE on new key creates value at correct size" {
        R 0 FLUSHALL
        R 0 SETRANGE $key 5 abc      ;# value len becomes 8: 3+8 = 11
        assert_equal 11 [get_slot_bytes $::OBJ_STRING $key_slot]
    }

    test "data-bytes: INCR digit-count growth (9 -> 10) is tracked" {
        R 0 FLUSHALL
        R 0 SET $key 9
        # 3 + 1 = 4
        assert_equal 4 [get_slot_bytes $::OBJ_STRING $key_slot]

        R 0 INCR $key
        # 3 + 2 = 5 (value is now "10")
        assert_equal 5 [get_slot_bytes $::OBJ_STRING $key_slot]
    }

    test "data-bytes: DECR digit shrink (10 -> 9) is tracked" {
        R 0 FLUSHALL
        R 0 SET $key 10
        # 3 + 2 = 5
        assert_equal 5 [get_slot_bytes $::OBJ_STRING $key_slot]

        R 0 DECR $key
        # value is "9": 3 + 1 = 4
        assert_equal 4 [get_slot_bytes $::OBJ_STRING $key_slot]
    }

    test "data-bytes: INCRBYFLOAT goes through dbReplaceValue" {
        R 0 FLUSHALL
        R 0 SET $key 1
        # 3 + 1 = 4
        assert_equal 4 [get_slot_bytes $::OBJ_STRING $key_slot]

        R 0 INCRBYFLOAT $key 0.5
        # value is "1.5": 3 + 3 = 6
        assert_equal 6 [get_slot_bytes $::OBJ_STRING $key_slot]
    }

    test "data-bytes: SETBIT extends SDS as needed" {
        R 0 FLUSHALL
        R 0 SETBIT $key 0 1
        # SDS length becomes 1 byte: 3+1 = 4
        assert_equal 4 [get_slot_bytes $::OBJ_STRING $key_slot]

        R 0 SETBIT $key 23 1
        # SDS length becomes 3 bytes: 3+3 = 6
        assert_equal 6 [get_slot_bytes $::OBJ_STRING $key_slot]
    }

    test "data-bytes: type transition string -> list drops string contribution" {
        R 0 FLUSHALL
        R 0 SET $key value
        # Before: 8 bytes attributed to string.
        assert_equal 8 [get_slot_bytes $::OBJ_STRING $key_slot]

        R 0 DEL $key
        R 0 RPUSH $key element
        # After: not currently tracked (lists not wired up); string drops to 0,
        # list stays 0. The point is that we did not double-count or leak.
        assert_equal 0 [get_slot_bytes $::OBJ_STRING $key_slot]
        assert_equal 0 [get_slot_bytes $::OBJ_LIST $key_slot]
    }

    test "data-bytes: UNLINK (async delete) subtracts correctly" {
        R 0 FLUSHALL
        R 0 SET $key value
        assert_equal 8 [get_slot_bytes $::OBJ_STRING $key_slot]

        R 0 UNLINK $key
        assert_equal 0 [get_slot_bytes $::OBJ_STRING $key_slot]
    }

    test "data-bytes: MSET updates the slot" {
        R 0 FLUSHALL
        # Use hash tags so both keys land in the same slot
        set k1 "{FOO}k1"
        set k2 "{FOO}k2"

        R 0 MSET $k1 hello $k2 world
        # {FOO}k1(7)+hello(5)=12, {FOO}k2(7)+world(5)=12 -> same slot, total 24
        assert_equal 24 [get_slot_bytes $::OBJ_STRING $key_slot]
    }

    test "data-bytes: RENAME within same slot updates accounting" {
        R 0 FLUSHALL
        set src "{FOO}src"
        set dst "{FOO}dst"

        R 0 SET $src value
        # {FOO}src(8) + value(5) = 13
        assert_equal 13 [get_slot_bytes $::OBJ_STRING $key_slot]

        R 0 RENAME $src $dst
        # src gone, dst has: {FOO}dst(8) + value(5) = 13
        assert_equal 13 [get_slot_bytes $::OBJ_STRING $key_slot]
    }

    test "data-bytes: key expiration subtracts correctly" {
        R 0 FLUSHALL
        R 0 SET $key value PX 50
        assert_equal 8 [get_slot_bytes $::OBJ_STRING $key_slot]

        after 100
        R 0 GET $key
        assert_equal 0 [get_slot_bytes $::OBJ_STRING $key_slot]
    }
}

# -----------------------------------------------------------------------------
# Bulk wipe semantics.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]

    test "data-bytes: FLUSHDB clears the state metric" {
        R 0 FLUSHALL
        R 0 SET $key value
        assert_equal 8 [get_slot_bytes $::OBJ_STRING $key_slot]

        R 0 FLUSHDB
        assert_equal 0 [get_slot_bytes $::OBJ_STRING $key_slot]
    }

    test "data-bytes: FLUSHALL clears the state metric" {
        R 0 SET $key value
        assert_equal 8 [get_slot_bytes $::OBJ_STRING $key_slot]

        R 0 FLUSHALL
        assert_equal 0 [get_slot_bytes $::OBJ_STRING $key_slot]
    }
}

# -----------------------------------------------------------------------------
# CONFIG RESETSTAT preserves data-bytes (state, not cumulative).
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster} overrides {cluster-slot-stats-enabled yes}} {
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]

    test "data-bytes: CONFIG RESETSTAT preserves data-bytes state metric" {
        R 0 FLUSHALL
        R 0 SET $key value
        assert_equal 8 [get_slot_bytes $::OBJ_STRING $key_slot]

        R 0 CONFIG RESETSTAT

        # data-bytes still reflects current key memory usage.
        assert_equal 8 [get_slot_bytes $::OBJ_STRING $key_slot]
        # cpu-usec / network-bytes-* should have been reset.
        set stats [get_slot_stats $key_slot]
        assert_equal 0 [dict get $stats cpu-usec]
        assert_equal 0 [dict get $stats network-bytes-in]
    }
}

# -----------------------------------------------------------------------------
# ORDERBY data-bytes.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {
    test "data-bytes: ORDERBY data-bytes DESC sorts by total per-slot bytes" {
        R 0 FLUSHALL
        R 0 SET key:small a
        R 0 SET key:large [string repeat z 200]

        set out [R 0 CLUSTER SLOT-STATS ORDERBY data-bytes LIMIT 2 DESC]
        set first_total  [dict get [lindex [lindex $out 0] 1] data-bytes string]
        set second_total [dict get [lindex [lindex $out 1] 1] data-bytes string]
        assert {$first_total >= $second_total}
        assert {$first_total > 0}
    }

    test "data-bytes: ORDERBY data-bytes is allowed without cluster-slot-stats-enabled" {
        R 0 CONFIG SET cluster-slot-stats-enabled no
        R 0 FLUSHALL
        R 0 SET FOO value
        set out [R 0 CLUSTER SLOT-STATS ORDERBY data-bytes LIMIT 1 DESC]
        assert_equal 1 [llength $out]
        R 0 CONFIG SET cluster-slot-stats-enabled yes
    }
}
