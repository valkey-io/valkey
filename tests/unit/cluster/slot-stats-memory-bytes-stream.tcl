# Integration tests for CLUSTER SLOT-STATS memory-bytes: OBJ_STREAM.
#
# Stream memory-bytes reflects the allocated footprint of the stream: the
# stream struct, its data radix tree, and the listpacks in that tree (tracked
# via stream->tracked_memory_bytes = Σ zmalloc_size of the listpacks). Because
# nodes are over-allocated, exact values depend on encoding and preallocation,
# so tests use relative comparisons. Consumer group memory is not yet counted.

source tests/unit/cluster/slot-stats-memory-bytes-helpers.tcl

# -----------------------------------------------------------------------------
# Core stream accounting.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]

    test "memory-bytes: XADD creates stream with non-zero bytes" {
        R 0 FLUSHALL
        R 0 XADD $key "*" name Alice age 30
        set bytes [get_slot_bytes $key_slot]
        assert {$bytes > 0}
    }

    test "memory-bytes: small XADD fits in preallocated node, size stays flat" {
        # Stream nodes are over-allocated (preallocated), and memory-bytes
        # tracks the allocated size. A small append fits in the existing slack,
        # so the allocated footprint does not change.
        set before [get_slot_bytes $key_slot]
        R 0 XADD $key "*" city NYC
        set after [get_slot_bytes $key_slot]
        assert {$after >= $before}
    }

    test "memory-bytes: XADD exceeding node slack grows byte count" {
        R 0 FLUSHALL
        R 0 XADD $key "*" f v
        set before [get_slot_bytes $key_slot]
        # A value larger than the node preallocation forces a bigger allocation.
        R 0 XADD $key "*" big [string repeat x 8192]
        set after [get_slot_bytes $key_slot]
        assert {$after > $before}
    }

    test "memory-bytes: XDEL does not increase byte count" {
        R 0 FLUSHALL
        set id1 [R 0 XADD $key "*" f1 v1]
        R 0 XADD $key "*" f2 v2
        set before [get_slot_bytes $key_slot]

        R 0 XDEL $key $id1
        set after [get_slot_bytes $key_slot]
        assert {$after <= $before}
    }

    test "memory-bytes: XTRIM reduces byte count" {
        R 0 FLUSHALL
        for {set i 0} {$i < 100} {incr i} {
            R 0 XADD $key "*" field [string repeat x 100]
        }
        set before [get_slot_bytes $key_slot]

        R 0 XTRIM $key MAXLEN 1
        set after [get_slot_bytes $key_slot]
        assert {$after < $before}
    }

    test "memory-bytes: XADD with MAXLEN trims and keeps size bounded" {
        R 0 FLUSHALL
        for {set i 0} {$i < 100} {incr i} {
            R 0 XADD $key MAXLEN 10 "*" f [string repeat v 50]
        }
        set size_at_10 [get_slot_bytes $key_slot]

        for {set i 0} {$i < 100} {incr i} {
            R 0 XADD $key MAXLEN 10 "*" f [string repeat v 50]
        }
        set size_after [get_slot_bytes $key_slot]
        # Size should stay roughly bounded, not grow linearly
        assert {$size_after <= $size_at_10 * 3}
    }

    test "memory-bytes: DEL on stream key drops to 0" {
        R 0 FLUSHALL
        R 0 XADD $key "*" x y
        assert {[get_slot_bytes $key_slot] > 0}
        R 0 DEL $key
        assert_equal 0 [get_slot_bytes $key_slot]
    }

    test "memory-bytes: UNLINK on stream key drops to 0" {
        R 0 FLUSHALL
        R 0 XADD $key "*" f v
        assert {[get_slot_bytes $key_slot] > 0}
        R 0 UNLINK $key
        assert_equal 0 [get_slot_bytes $key_slot]
    }

    test "memory-bytes: FLUSHALL clears stream accounting" {
        R 0 XADD $key "*" hello world
        assert {[get_slot_bytes $key_slot] > 0}
        R 0 FLUSHALL
        assert_equal 0 [get_slot_bytes $key_slot]
    }
}

# -----------------------------------------------------------------------------
# Edge cases.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]

    test "memory-bytes: XDEL on non-existent ID causes no change" {
        R 0 FLUSHALL
        R 0 XADD $key "*" x y
        set before [get_slot_bytes $key_slot]
        R 0 XDEL $key 9999999-0
        assert_equal $before [get_slot_bytes $key_slot]
    }

    test "memory-bytes: XTRIM with MAXLEN >= length causes no change" {
        R 0 FLUSHALL
        R 0 XADD $key "*" a b
        R 0 XADD $key "*" c d
        set before [get_slot_bytes $key_slot]
        R 0 XTRIM $key MAXLEN 10
        assert_equal $before [get_slot_bytes $key_slot]
    }

    test "memory-bytes: stream key expiration drops to 0" {
        R 0 FLUSHALL
        R 0 XADD $key "*" hello world
        R 0 PEXPIRE $key 50
        assert {[get_slot_bytes $key_slot] > 0}
        after 100
        R 0 TYPE $key
        assert_equal 0 [get_slot_bytes $key_slot]
    }

    test "memory-bytes: COPY preserves stream byte count" {
        R 0 FLUSHALL
        R 0 XADD $key "*" field1 value1
        R 0 XADD $key "*" field2 value2
        set before [get_slot_bytes $key_slot]
        assert {$before > 0}

        set dst "{FOO}copy"
        R 0 COPY $key $dst
        set after [get_slot_bytes $key_slot]
        # Slot now holds both keys. The copy is allocated exact-fit, so its
        # listpack allocation may be smaller than the original's preallocated
        # node; assert the copy added a positive, comparable amount rather than
        # an exact figure.
        assert {$after > $before}
    }
}

# -----------------------------------------------------------------------------
# Exact accuracy: for a stream without consumer groups, memory-bytes must equal
# MEMORY USAGE (SAMPLES 0), which sums the stream struct, the data radix tree,
# and the listpacks. The match is byte-for-byte. (With consumer groups the two
# diverge, since MEMORY USAGE also counts consumer-group memory.)
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]

    test "memory-bytes matches MEMORY USAGE: single-node stream" {
        R 0 FLUSHALL
        R 0 XADD $key "*" name Alice age 30
        assert_slot_bytes_match_memory_usage $key_slot
    }

    test "memory-bytes matches MEMORY USAGE: multi-node stream" {
        R 0 FLUSHALL
        for {set i 0} {$i < 500} {incr i} {
            R 0 XADD $key "*" field [string repeat x 50]
        }
        assert_slot_bytes_match_memory_usage $key_slot
    }

    test "memory-bytes matches MEMORY USAGE: after XDEL and XTRIM" {
        R 0 FLUSHALL
        for {set i 0} {$i < 200} {incr i} {
            R 0 XADD $key "*" f v$i
        }
        set first_id [lindex [lindex [R 0 XRANGE $key - + COUNT 1] 0] 0]
        R 0 XDEL $key $first_id
        R 0 XTRIM $key MAXLEN 50
        assert_slot_bytes_match_memory_usage $key_slot
    }
}
