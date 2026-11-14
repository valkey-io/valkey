# Reply blocking caps replica propagation at the durable offset: with
# appendfsync=always and the AOF flush offloaded to a BIO thread, a replica
# must never receive a write whose AOF record is not yet fsynced on the
# primary, otherwise a crash before the fsync followed by a failover would
# promote data the primary's AOF does not have.
#
# DEBUG reply-blocking-pause aof freezes the durable offset reported to reply
# blocking (the fsync itself still runs), which makes "held" vs "released"
# deterministic. Run this file with --io-threads as well to cover the IO-thread
# write path.

set overrides {
    appendonly yes
    appendfsync always
    bio-aof-offload-enabled yes
    repl-ping-replica-period 3600
    save ""
}

# Read the replica's socket without blocking; used to prove a held reply
# has not been sent.
proc read_early_reply {rd} {
    set fd [$rd channel]
    fconfigure $fd -blocking 0
    set data [read $fd]
    fconfigure $fd -blocking 1
    return $data
}

proc replica_offset_on_primary {primary} {
    set info [$primary info replication]
    assert {[regexp {slave0:.*offset=([0-9]+)} $info -> ofs]}
    return $ofs
}

foreach repl_compression {no yes} {
    start_server [list tags {"repl durability external:skip"} overrides [concat $overrides [list repl-compression $repl_compression]]] {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        start_server [list overrides [list save "" repl-compression $repl_compression]] {
            set replica [srv 0 client]

            $replica replicaof $primary_host $primary_port
            wait_replica_online $primary
            wait_for_ofs_sync $primary $replica

            test "Replica converges with reply blocking enabled (compression=$repl_compression)" {
                for {set i 0} {$i < 100} {incr i} {
                    $primary set cap:base:$i $i
                }
                wait_for_ofs_sync $primary $replica
                assert_equal 99 [$replica get cap:base:99]
            }

            test "Replica does not receive a write until it is durable (compression=$repl_compression)" {
                wait_for_ofs_sync $primary $replica
                set before [status $replica master_repl_offset]

                $primary debug reply-blocking-pause aof

                set rd [valkey_deferring_client -1]
                $rd set cap:held value
                assert_equal "" [read_early_reply $rd]

                # The primary advanced, the replica must not have. The primary's
                # view of the replica offset only updates on REPLCONF ACK, so it
                # can lag behind but must never exceed the pre-write offset.
                assert {[status $primary master_repl_offset] > $before}
                after 200
                assert_equal $before [status $replica master_repl_offset]
                assert {[replica_offset_on_primary $primary] <= $before}
                assert_equal {} [$replica get cap:held]

                $primary debug reply-blocking-resume aof
                assert_equal "OK" [$rd read]
                $rd close

                wait_for_ofs_sync $primary $replica
                assert_equal "value" [$replica get cap:held]
            }

            test "Cap is byte-precise within one replication buffer block (compression=$repl_compression)" {
                wait_for_ofs_sync $primary $replica

                # k1 becomes durable, then k2 is fed into the same block while
                # the durable offset is frozen: the replica must get exactly k1.
                $primary set cap:k1 v1
                wait_for_ofs_sync $primary $replica
                set k1_ofs [status $primary master_repl_offset]

                $primary debug reply-blocking-pause aof
                set rd [valkey_deferring_client -1]
                $rd set cap:k2 v2
                assert_equal "" [read_early_reply $rd]

                after 200
                assert_equal "v1" [$replica get cap:k1]
                assert_equal {} [$replica get cap:k2]
                assert_equal $k1_ofs [status $replica master_repl_offset]

                $primary debug reply-blocking-resume aof
                assert_equal "OK" [$rd read]
                $rd close
                wait_for_ofs_sync $primary $replica
                assert_equal "v2" [$replica get cap:k2]
            }

            test "Held bytes are released when the durable offset advances, not on a later write (compression=$repl_compression)" {
                wait_for_ofs_sync $primary $replica
                $primary debug reply-blocking-pause aof
                set rd [valkey_deferring_client -1]
                $rd set cap:release value
                assert_equal "" [read_early_reply $rd]
                after 100
                assert_equal {} [$replica get cap:release]

                # Resume with no further traffic: the wake-up must come from the
                # durable offset moving, otherwise the replica stays parked.
                $primary debug reply-blocking-resume aof
                assert_equal "OK" [$rd read]
                $rd close
                wait_for_condition 50 100 {
                    [$replica get cap:release] eq "value"
                } else {
                    fail "replica was not woken when the durable offset advanced"
                }
                wait_for_ofs_sync $primary $replica
            }

            test "Large values spanning several blocks are capped and delivered intact (compression=$repl_compression)" {
                wait_for_ofs_sync $primary $replica
                set big [string repeat "x" 300000]

                $primary debug reply-blocking-pause aof
                set rd [valkey_deferring_client -1]
                $rd set cap:big $big
                assert_equal "" [read_early_reply $rd]
                after 200
                assert_equal 0 [$replica exists cap:big]

                $primary debug reply-blocking-resume aof
                assert_equal "OK" [$rd read]
                $rd close
                wait_for_ofs_sync $primary $replica
                assert_equal [string length $big] [$replica strlen cap:big]
            }

            test "Randomized pause/write/resume never leaks a held write and always converges (compression=$repl_compression)" {
                # Random sizes walk the durable boundary across block edges,
                # exercising the boundary comparison at arbitrary positions.
                # Debug asserts in the write path fail this test on an
                # over-send; the read below fails it on a leak.
                for {set i 0} {$i < 30} {incr i} {
                    wait_for_ofs_sync $primary $replica
                    set len [expr {1 + int(rand() * 40000)}]
                    set val [string repeat "r" $len]

                    $primary debug reply-blocking-pause aof
                    set rd [valkey_deferring_client -1]
                    $rd set cap:rand:$i $val
                    assert_equal "" [read_early_reply $rd]
                    assert_equal 0 [$replica exists cap:rand:$i]

                    $primary debug reply-blocking-resume aof
                    assert_equal "OK" [$rd read]
                    $rd close
                    wait_for_condition 50 100 {
                        [$replica strlen cap:rand:$i] == $len
                    } else {
                        fail "replica did not converge on iteration $i"
                    }
                }
                wait_for_ofs_sync $primary $replica
            }

            test "Disabling reply blocking releases held replication bytes (compression=$repl_compression)" {
                wait_for_ofs_sync $primary $replica
                $primary debug reply-blocking-pause aof
                set rd [valkey_deferring_client -1]
                $rd set cap:disable value
                assert_equal "" [read_early_reply $rd]
                after 100
                assert_equal {} [$replica get cap:disable]

                # Turning the feature off collapses the send limit to the
                # buffer tail; the parked replica must be woken.
                $primary config set bio-aof-offload-enabled no
                assert_equal "OK" [$rd read]
                $rd close
                wait_for_condition 50 100 {
                    [$replica get cap:disable] eq "value"
                } else {
                    fail "replica was not woken when reply blocking was disabled"
                }
                wait_for_ofs_sync $primary $replica

                $primary debug reply-blocking-resume aof
                $primary config set bio-aof-offload-enabled yes
            }

            test "Partial resync after a disconnect while a write is held (compression=$repl_compression)" {
                wait_for_ofs_sync $primary $replica
                set sync_partial_before [status $primary sync_partial_ok]

                $primary debug reply-blocking-pause aof
                set rd [valkey_deferring_client -1]
                $rd set cap:psync value
                assert_equal "" [read_early_reply $rd]
                after 100
                assert_equal {} [$replica get cap:psync]

                # Drop the link: the replica reconnects asking for the offset it
                # actually received, which is durable, so it must PSYNC and it
                # must still not see the held write until resume.
                $primary client kill type replica
                wait_replica_online $primary
                wait_for_condition 50 100 {
                    [status $primary sync_partial_ok] == $sync_partial_before + 1
                } else {
                    fail "replica did not partial resync"
                }
                after 100
                assert_equal {} [$replica get cap:psync]

                $primary debug reply-blocking-resume aof
                assert_equal "OK" [$rd read]
                $rd close
                wait_for_ofs_sync $primary $replica
                assert_equal "value" [$replica get cap:psync]

            test "Parked replica is not written to repeatedly (compression=$repl_compression)" {
                # A replica whose cursor sits at the durable limit must report
                # nothing pending, so the event loop does not attempt writes
                # against it. Exact positioning of the limit on a block boundary
                # is covered by the ReplicaSendLimitTest gtests; here we only
                # check the end-to-end consequence.
                wait_for_ofs_sync $primary $replica
                $primary set cap:park:pad [string repeat "p" 16384]
                wait_for_ofs_sync $primary $replica
                set before [status $primary total_writes_processed]

                $primary debug reply-blocking-pause aof
                set rd [valkey_deferring_client -1]
                $rd set cap:park:next value
                assert_equal "" [read_early_reply $rd]
                after 300
                assert_equal {} [$replica get cap:park:next]
                set writes [expr {[status $primary total_writes_processed] - $before}]
                assert {$writes < 20}

                $primary debug reply-blocking-resume aof
                assert_equal "OK" [$rd read]
                $rd close
                wait_for_ofs_sync $primary $replica
                assert_equal "value" [$replica get cap:park:next]
            }

            test "Toggling appendonly off and on with a replica attached converges (compression=$repl_compression)" {
                wait_for_ofs_sync $primary $replica
                $primary config set appendonly no
                $primary set cap:aofoff v
                wait_for_ofs_sync $primary $replica

                # Re-enabling AOF resets the durable offset to 0 until the
                # rewrite completes, so the send limit is briefly before every
                # retained block. The rewrite is too fast here to observe the
                # stall; this checks the path recovers and converges.
                $primary config set appendonly yes
                $primary set cap:aofon v
                wait_for_condition 100 100 {
                    [status $primary aof_rewrite_in_progress] == 0
                } else {
                    fail "AOF rewrite did not finish"
                }
                wait_for_condition 50 100 {
                    [$replica get cap:aofon] eq "v"
                } else {
                    fail "replica did not converge after the AOF rewrite"
                }
                wait_for_ofs_sync $primary $replica
                assert_equal "yes" [lindex [$primary config get appendonly] 1]
            }

            test "FAILOVER completes with the cap active (compression=$repl_compression)" {
                wait_for_ofs_sync $primary $replica
                set replica_host [srv 0 host]
                set replica_port [srv 0 port]
                set primary_host [srv -1 host]
                set primary_port [srv -1 port]

                $primary set cap:failover v
                $primary failover to $replica_host $replica_port
                wait_for_condition 100 100 {
                    [status $primary master_failover_state] eq "no-failover" &&
                    [status $primary role] eq "slave" &&
                    [status $replica role] eq "master"
                } else {
                    fail "failover did not complete"
                }
                assert_equal "v" [$replica get cap:failover]

                # Restore roles for the remaining tests.
                $replica failover to $primary_host $primary_port
                wait_for_condition 100 100 {
                    [status $primary role] eq "master" && [status $replica role] eq "slave"
                } else {
                    fail "failback did not complete"
                }
                wait_for_condition 50 100 {
                    [status $replica master_link_status] eq "up"
                } else {
                    fail "replica link did not come back up"
                }
                wait_for_ofs_sync $primary $replica
            }
            }
        }
    }
}

# Role changes. A primary with reply blocking enabled is demoted under a new
# primary with a much lower replication offset, keeps its sub-replica through
# the resync, and is then promoted again. Its replication offset (and so its
# AOF durable offset) is now far below what it was, while any state the cap
# keeps from its previous life as a primary still holds the old, higher values.
# The sub-replica must keep converging after the promotion.
start_server [list tags {"repl durability external:skip"} overrides $overrides] {
    set a [srv 0 client]
    set a_host [srv 0 host]
    set a_port [srv 0 port]

    start_server {overrides {save ""}} {
        set c [srv 0 client]

        start_server {overrides {save ""}} {
            set b [srv 0 client]
            set b_host [srv 0 host]
            set b_port [srv 0 port]

            test "Sub-replica keeps converging after its primary is demoted and re-promoted at a lower offset" {
                # A primary, C replica of A. Push A's offset well past anything
                # B will ever have.
                $c replicaof $a_host $a_port
                wait_replica_online $a
                for {set i 0} {$i < 3000} {incr i} {
                    $a set role:a:$i [string repeat "a" 64]
                }
                wait_for_ofs_sync $a $c
                set a_high_offset [status $a master_repl_offset]

                # B is a small independent primary.
                $b set role:b:1 x
                set b_offset [status $b master_repl_offset]
                assert {$b_offset < $a_high_offset / 10}

                # Demote A under B. A full syncs (different replid) and adopts
                # B's offset; C is disconnected on A's replid change and
                # resyncs from A as a sub-replica.
                $a replicaof $b_host $b_port
                wait_for_condition 100 100 {
                    [status $a master_link_status] eq "up"
                } else {
                    fail "A did not sync from B"
                }
                assert {[status $a master_repl_offset] < $a_high_offset / 10}
                wait_replica_online $a
                wait_for_ofs_sync $a $c
                assert_equal "x" [$c get role:b:1]

                # While A is a replica the cap is off; B's writes flow through
                # A to C.
                $b set role:b:2 y
                wait_for_condition 50 100 {
                    [$c get role:b:2] eq "y"
                } else {
                    fail "chained replication A->C broken while A is a replica"
                }

                # Promote A. The cap is active again at A's new, low offset.
                $a replicaof no one
                wait_for_condition 50 100 {
                    [status $a role] eq "master"
                } else {
                    fail "A did not become primary"
                }
                wait_for_ofs_sync $a $c

                # Every write must still reach C: the fsync completion must
                # re-arm C even though A's durable offset is now lower than
                # at any point during A's first life as a primary.
                for {set i 0} {$i < 5} {incr i} {
                    $a set role:after:$i v$i
                    wait_for_condition 50 100 {
                        [$c get role:after:$i] eq "v$i"
                    } else {
                        fail "sub-replica stopped converging after re-promotion (write $i)"
                    }
                }
                wait_for_ofs_sync $a $c

                # And the cap itself still holds after the role change.
                $a debug reply-blocking-pause aof
                set rd [valkey_deferring_client -2]
                $rd set role:after:held value
                assert_equal "" [read_early_reply $rd]
                after 200
                assert_equal {} [$c get role:after:held]
                $a debug reply-blocking-resume aof
                assert_equal "OK" [$rd read]
                $rd close
                wait_for_ofs_sync $a $c
                assert_equal "value" [$c get role:after:held]
            }
        }
    }
}

# Full sync: the RDB snapshot bypasses the stream cap, so the primary drains
# any offloaded AOF flush before forking. These exercise every transfer path
# with reply blocking active and a write held at fork time.
foreach {diskless dual_channel} {no no yes no yes yes} {
    start_server [list tags {"repl durability external:skip"} overrides [concat $overrides [list repl-diskless-sync $diskless repl-diskless-sync-delay 0 dual-channel-replication-enabled $dual_channel]]] {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        for {set i 0} {$i < 50} {incr i} {
            $primary set cap:fs:$i $i
        }

        start_server [list overrides [list save "" dual-channel-replication-enabled $dual_channel]] {
            set replica [srv 0 client]

            test "Full sync with a held write at fork time completes (diskless=$diskless dual-channel=$dual_channel)" {
                $primary debug reply-blocking-pause aof
                set rd [valkey_deferring_client -1]
                $rd set cap:fs:held value
                assert_equal "" [read_early_reply $rd]

                $replica replicaof $primary_host $primary_port
                wait_replica_online $primary
                wait_for_condition 50 100 {
                    [status $replica master_link_status] eq "up"
                } else {
                    fail "replica did not come up"
                }
                assert_equal 49 [$replica get cap:fs:49]

                # The held write is only released to the stream on resume.
                $primary debug reply-blocking-resume aof
                assert_equal "OK" [$rd read]
                $rd close
                wait_for_ofs_sync $primary $replica
                assert_equal "value" [$replica get cap:fs:held]
                assert_equal [$primary dbsize] [$replica dbsize]
            }

            test "Writes after full sync are still capped (diskless=$diskless dual-channel=$dual_channel)" {
                wait_for_ofs_sync $primary $replica
                $primary debug reply-blocking-pause aof
                set rd [valkey_deferring_client -1]
                $rd set cap:fs:after value
                assert_equal "" [read_early_reply $rd]
                after 200
                assert_equal {} [$replica get cap:fs:after]

                $primary debug reply-blocking-resume aof
                assert_equal "OK" [$rd read]
                $rd close
                wait_for_ofs_sync $primary $replica
                assert_equal "value" [$replica get cap:fs:after]
            }
        }
    }
}
