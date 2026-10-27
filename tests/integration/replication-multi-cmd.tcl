# Tests for multi-command parsing on the replication stream.
# Verifies that pipelined commands replicated to a replica are parsed in
# batches into c->cmd_queue while keeping per-command qb_applied tracking
# (advanced by parsedCommand.input_bytes), so the replication offset stays
# exact and chained replication converges.

# Assert primary and replica converge: same digest, dbsize, reploff.
proc assert_primary_replica_consistent {primary replica} {
    wait_for_ofs_sync $primary $replica
    assert_equal [$primary debug digest] [$replica debug digest]
    assert_equal [$primary dbsize] [$replica dbsize]
}

# Pipeline correctness on a single primary/replica pair.
start_server {tags {"repl external:skip"}} {
    start_server {} {
        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        set replica [srv 0 client]

        $replica replicaof $primary_host $primary_port
        wait_for_sync $replica
        wait_for_ofs_sync $primary $replica

        test {Multi-command parsing: large SET pipeline keeps reploff exact} {
            set rd [valkey_deferring_client -1]
            for {set i 0} {$i < 500} {incr i} {
                $rd set key1 value1
                $rd set key2 value2
            }
            for {set i 0} {$i < 1000} {incr i} {
                $rd read
            }
            assert_primary_replica_consistent $primary $replica
            $rd close
        }

        test {Multi-command parsing: MULTI/EXEC across pipelined batch} {
            set rd [valkey_deferring_client -1]
            for {set i 0} {$i < 200} {incr i} {
                $rd multi
                $rd set key1 value1
                $rd set key2 value2
                $rd set key3 value3
                $rd exec
            }
            for {set i 0} {$i < 1000} {incr i} {
                $rd read
            }
            assert_primary_replica_consistent $primary $replica
            $rd close
        }

        test {Multi-command parsing: PROTO_MBULK_BIG_ARG in pipeline} {
            set rd [valkey_deferring_client -1]
            set big_string [string repeat X 100000]
            for {set i 0} {$i < 50} {incr i} {
                $rd set key1 $big_string
                $rd set key2 $big_string
            }
            for {set i 0} {$i < 100} {incr i} {
                $rd read
            }
            assert_primary_replica_consistent $primary $replica
            $rd close
        }

        test {Multi-command parsing: pipeline survives PSYNC reconnect} {
            set rd [valkey_deferring_client -1]
            for {set i 0} {$i < 200} {incr i} {
                $rd set kx$i vx$i
            }
            for {set i 0} {$i < 200} {incr i} {
                $rd read
            }

            # Force replica to drop the primary connection.
            $replica client kill type primary
            wait_for_sync $replica

            # Push another batch of pipelined writes after the reconnect.
            for {set i 0} {$i < 200} {incr i} {
                $rd set ky$i vy$i
            }
            for {set i 0} {$i < 200} {incr i} {
                $rd read
            }

            assert_primary_replica_consistent $primary $replica
            $rd close
        }
    }
}

# Chained replication (subreplica -> replica -> primary).
# Verifies the replica forwards the exact byte stream to its own replicas.
foreach policy {require-fork require-forkless} {
start_server {tags {"repl external:skip"}} {
    start_server [list overrides [list forkless-infrastructure-enabled yes forkless-policy $policy \
                                       repl-diskless-sync yes repl-diskless-sync-delay 0]] {
        start_server {} {
            set primary [srv -2 client]
            set primary_host [srv -2 host]
            set primary_port [srv -2 port]
            set replica [srv -1 client]
            set replica_host [srv -1 host]
            set replica_port [srv -1 port]
            set subreplica [srv 0 client]

            $replica replicaof $primary_host $primary_port
            $subreplica replicaof $replica_host $replica_port
            wait_for_sync $replica
            wait_for_sync $subreplica
            wait_for_ofs_sync $primary $replica
            wait_for_ofs_sync $replica $subreplica

            test "Chained replication ($policy): pipelined SETs converge on all 3 nodes" {
                set rd [valkey_deferring_client -2]
                for {set i 0} {$i < 500} {incr i} {
                    $rd set key1 value1
                    $rd set key2 value2
                }
                for {set i 0} {$i < 1000} {incr i} {
                    $rd read
                }
                assert_primary_replica_consistent $primary $replica
                assert_primary_replica_consistent $replica $subreplica
                $rd close
            }

            test "Chained replication ($policy): MULTI/EXEC propagates correctly" {
                set rd [valkey_deferring_client -2]
                for {set i 0} {$i < 200} {incr i} {
                    $rd multi
                    $rd set key1 value1
                    $rd set key2 value2
                    $rd set key3 value3
                    $rd exec
                }
                for {set i 0} {$i < 1000} {incr i} {
                    $rd read
                }
                assert_primary_replica_consistent $primary $replica
                assert_primary_replica_consistent $replica $subreplica
                $rd close
            }

            test "Chained replication ($policy): PROTO_MBULK_BIG_ARG in pipeline" {
                set rd [valkey_deferring_client -2]
                set big_string [string repeat X 100000]
                for {set i 0} {$i < 50} {incr i} {
                    $rd set key1 $big_string
                    $rd set key2 $big_string
                }
                for {set i 0} {$i < 100} {incr i} {
                    $rd read
                }
                assert_primary_replica_consistent $primary $replica
                assert_primary_replica_consistent $replica $subreplica
                $rd close
            }

            test "Chained replication ($policy): pipeline survives PSYNC reconnect" {
                set rd [valkey_deferring_client -2]
                for {set i 0} {$i < 200} {incr i} {
                    $rd set kx$i vx$i
                }
                for {set i 0} {$i < 200} {incr i} {
                    $rd read
                }

                # Force replica to drop the primary connection.
                $replica client kill type primary
                wait_for_sync $replica

                # Push another batch of pipelined writes after the reconnect.
                for {set i 0} {$i < 200} {incr i} {
                    $rd set ky$i vy$i
                }
                for {set i 0} {$i < 200} {incr i} {
                    $rd read
                }

                assert_primary_replica_consistent $primary $replica
                assert_primary_replica_consistent $replica $subreplica
                $rd close
            }

            test "Chained replication ($policy): pipeline survives subreplica<-replica reconnect" {
                set rd [valkey_deferring_client -2]
                for {set i 0} {$i < 200} {incr i} {
                    $rd set kp$i vp$i
                }
                for {set i 0} {$i < 200} {incr i} {
                    $rd read
                }

                # Force replica to drop the primary connection.
                $subreplica client kill type primary
                wait_for_sync $subreplica

                # Push another batch of pipelined writes after the reconnect.
                for {set i 0} {$i < 200} {incr i} {
                    $rd set kq$i vq$i
                }
                for {set i 0} {$i < 200} {incr i} {
                    $rd read
                }

                assert_primary_replica_consistent $primary $replica
                assert_primary_replica_consistent $replica $subreplica
                $rd close
            }

            test "Chained replication ($policy): subreplica full sync under write load loses no writes" {
                populate 20000 fill: 10 -2
                wait_for_ofs_sync $primary $replica
                
                # force the subreplica to do a fresh full sync.
                $subreplica replicaof no one
                $subreplica flushall

                set loglines [count_log_lines -1]
                $subreplica replicaof $replica_host $replica_port
                for {set i 0} {$i < 20000} {incr i} { $primary set load:$i $i }
                wait_for_sync $subreplica 500 100

                if {$policy eq "require-forkless"} {
                    wait_for_log_messages -1 {"*target: replicas sockets*method: forkless*"} $loglines 1 1
                }
                assert_primary_replica_consistent $primary $replica
                assert_primary_replica_consistent $replica $subreplica
                for {set i 0} {$i < 20000} {incr i} {
                    assert_equal $i [$subreplica get load:$i]
                }
            }
        }
    }
}
}
