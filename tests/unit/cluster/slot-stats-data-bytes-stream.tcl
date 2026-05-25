# Integration tests for CLUSTER SLOT-STATS data-bytes: OBJ_STREAM.
#
# Stream data-bytes uses tracked_data_bytes = Σ lpBytes for all listpacks
# in the stream's rax. This includes listpack overhead (headers, encoding
# metadata) so exact values depend on encoding. Tests use relative comparisons.

source tests/unit/cluster/slot-stats-data-bytes-helpers.tcl

# -----------------------------------------------------------------------------
# Core stream accounting.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]

    test "data-bytes: XADD creates stream with non-zero bytes" {
        R 0 FLUSHALL
        R 0 XADD $key "*" name Alice age 30
        set bytes [get_slot_bytes $::OBJ_STREAM $key_slot]
        assert {$bytes > 0}
    }

    test "data-bytes: multiple XADDs increase byte count" {
        set before [get_slot_bytes $::OBJ_STREAM $key_slot]
        R 0 XADD $key "*" city NYC
        set after [get_slot_bytes $::OBJ_STREAM $key_slot]
        assert {$after > $before}
    }

    test "data-bytes: XDEL does not increase byte count" {
        R 0 FLUSHALL
        set id1 [R 0 XADD $key "*" f1 v1]
        R 0 XADD $key "*" f2 v2
        set before [get_slot_bytes $::OBJ_STREAM $key_slot]

        R 0 XDEL $key $id1
        set after [get_slot_bytes $::OBJ_STREAM $key_slot]
        assert {$after <= $before}
    }

    test "data-bytes: XTRIM reduces byte count" {
        R 0 FLUSHALL
        for {set i 0} {$i < 100} {incr i} {
            R 0 XADD $key "*" field [string repeat x 100]
        }
        set before [get_slot_bytes $::OBJ_STREAM $key_slot]

        R 0 XTRIM $key MAXLEN 1
        set after [get_slot_bytes $::OBJ_STREAM $key_slot]
        assert {$after < $before}
    }

    test "data-bytes: XADD with MAXLEN trims and keeps size bounded" {
        R 0 FLUSHALL
        for {set i 0} {$i < 100} {incr i} {
            R 0 XADD $key MAXLEN 10 "*" f [string repeat v 50]
        }
        set size_at_10 [get_slot_bytes $::OBJ_STREAM $key_slot]

        for {set i 0} {$i < 100} {incr i} {
            R 0 XADD $key MAXLEN 10 "*" f [string repeat v 50]
        }
        set size_after [get_slot_bytes $::OBJ_STREAM $key_slot]
        # Size should stay roughly bounded, not grow linearly
        assert {$size_after <= $size_at_10 * 3}
    }

    test "data-bytes: DEL on stream key drops to 0" {
        R 0 FLUSHALL
        R 0 XADD $key "*" x y
        assert {[get_slot_bytes $::OBJ_STREAM $key_slot] > 0}
        R 0 DEL $key
        assert_equal 0 [get_slot_bytes $::OBJ_STREAM $key_slot]
    }

    test "data-bytes: UNLINK on stream key drops to 0" {
        R 0 FLUSHALL
        R 0 XADD $key "*" f v
        assert {[get_slot_bytes $::OBJ_STREAM $key_slot] > 0}
        R 0 UNLINK $key
        assert_equal 0 [get_slot_bytes $::OBJ_STREAM $key_slot]
    }

    test "data-bytes: FLUSHALL clears stream accounting" {
        R 0 XADD $key "*" hello world
        assert {[get_slot_bytes $::OBJ_STREAM $key_slot] > 0}
        R 0 FLUSHALL
        assert_equal 0 [get_slot_bytes $::OBJ_STREAM $key_slot]
    }
}

# -----------------------------------------------------------------------------
# Edge cases.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]

    test "data-bytes: XDEL on non-existent ID causes no change" {
        R 0 FLUSHALL
        R 0 XADD $key "*" x y
        set before [get_slot_bytes $::OBJ_STREAM $key_slot]
        R 0 XDEL $key 9999999-0
        assert_equal $before [get_slot_bytes $::OBJ_STREAM $key_slot]
    }

    test "data-bytes: XTRIM with MAXLEN >= length causes no change" {
        R 0 FLUSHALL
        R 0 XADD $key "*" a b
        R 0 XADD $key "*" c d
        set before [get_slot_bytes $::OBJ_STREAM $key_slot]
        R 0 XTRIM $key MAXLEN 10
        assert_equal $before [get_slot_bytes $::OBJ_STREAM $key_slot]
    }

    test "data-bytes: stream key expiration drops to 0" {
        R 0 FLUSHALL
        R 0 XADD $key "*" hello world
        R 0 PEXPIRE $key 50
        assert {[get_slot_bytes $::OBJ_STREAM $key_slot] > 0}
        after 100
        R 0 TYPE $key
        assert_equal 0 [get_slot_bytes $::OBJ_STREAM $key_slot]
    }

    test "data-bytes: COPY preserves stream byte count" {
        R 0 FLUSHALL
        R 0 XADD $key "*" field1 value1
        R 0 XADD $key "*" field2 value2
        set before [get_slot_bytes $::OBJ_STREAM $key_slot]
        assert {$before > 0}

        set dst "{FOO}copy"
        R 0 COPY $key $dst
        set after [get_slot_bytes $::OBJ_STREAM $key_slot]
        # Slot now has both keys. The copy has same tracked_data_bytes
        # but different key length: key(3) vs {FOO}copy(9), delta = 6
        assert_equal [expr {$before * 2 + 6}] $after
    }
}
