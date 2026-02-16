start_server {tags {"repl external:skip"}} {
    start_server {overrides {save {}}} {
        set node0 [srv -1 client]
        set node0_host [srv -1 host]
        set node0_port [srv -1 port]
        set node1 [srv 0 client]
        set node1_host [srv 0 host]
        set node1_port [srv 0 port]

        test {Replica link with active-replica enabled} {
            $node0 config set active-replica yes
            $node0 config set multi-master yes
            $node0 config set replica-read-only no
            $node1 config set active-replica yes
            $node1 config set multi-master yes
            $node1 config set replica-read-only no

            $node1 replicaof $node0_host $node0_port
            wait_for_condition 100 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Initial replica link not established"
            }
        }

        test {RREPLAY LWW converges to latest write} {
            $node0 set mm:lww first
            after 25
            $node1 set mm:lww second

            wait_for_condition 100 100 {
                [$node0 get mm:lww] eq {second} &&
                [$node1 get mm:lww] eq {second}
            } else {
                fail "LWW convergence failed node0=[$node0 get mm:lww] node1=[$node1 get mm:lww]"
            }
        }

        test {RREPLAY blocks stream writes with generated side effects} {
            $node1 del mm:stream
            $node0 del mm:stream

            $node1 xadd mm:stream * f v1
            wait_for_condition 50 20 {
                [$node1 xlen mm:stream] == 1
            } else {
                fail "local stream write was not applied on node1"
            }

            after 100
            assert_equal 0 [$node0 xlen mm:stream]
        }

        test {RREPLAY blocks relative TTL commands} {
            $node0 set mm:ttl anchor
            wait_for_condition 100 50 {
                [$node1 get mm:ttl] eq {anchor}
            } else {
                fail "initial key did not replicate to node1"
            }

            assert_equal 1 [$node1 expire mm:ttl 120]
            after 100
            assert_equal -1 [$node0 ttl mm:ttl]
            assert {[$node1 ttl mm:ttl] > 0}
        }

        test {RREPLAY blocks risky RMW commands} {
            $node0 del mm:rmw:append mm:rmw:incr
            $node1 del mm:rmw:append mm:rmw:incr

            assert_equal 5 [$node1 append mm:rmw:append local]
            assert_equal 1 [$node1 incr mm:rmw:incr]
            after 100

            assert_equal "local" [$node1 get mm:rmw:append]
            assert_equal "1" [$node1 get mm:rmw:incr]
            assert_equal {} [$node0 get mm:rmw:append]
            assert_equal {} [$node0 get mm:rmw:incr]
        }

        test {MVCCRESTORE enforces stale protection} {
            $node1 set mm:mvcc base
            set payload [$node1 dump mm:mvcc]
            $node1 mvccrestore mm:mvcc 0 $payload 100 replace
            assert_equal "base" [$node1 get mm:mvcc]

            $node1 set mm:mvcc newer
            set payload_old [$node1 dump mm:mvcc]
            $node1 set mm:mvcc latest
            $node1 mvccrestore mm:mvcc 0 $payload_old 50 replace
            assert_equal "latest" [$node1 get mm:mvcc]
        }

        test {RREPLAY MSET applies fresh keys without dropping the full command} {
            $node0 set mm:mset:tmp1 old1
            $node0 set mm:mset:tmp2 keep
            set k1_payload [$node0 dump mm:mset:tmp1]
            set k2_payload [$node0 dump mm:mset:tmp2]
            $node0 del mm:mset:tmp1 mm:mset:tmp2 mm:mset:k1 mm:mset:k2
            set base_clock [s -1 mvcc_clock]
            set k1_ts [expr {$base_clock + 10}]
            set k2_ts [expr {$base_clock + 1000}]
            set replay_ts [expr {$base_clock + 500}]
            set client_info [$node0 client info]
            set dbid 0
            regexp {db=([0-9]+)} $client_info _ dbid
            $node0 mvccrestore mm:mset:k1 0 $k1_payload $k1_ts replace
            $node0 mvccrestore mm:mset:k2 0 $k2_payload $k2_ts replace

            assert_equal OK [$node0 replconf capa rreplay-peer]
            assert_equal OK [$node0 replconf uuid 1111111111111111111111111111111111111111]
            assert_equal 9001 [$node0 rreplay 2222222222222222222222222222222222222222 $dbid 9001 $replay_ts mset mm:mset:k1 new1 mm:mset:k2 new2]

            assert_equal "new1" [$node0 get mm:mset:k1]
            assert_equal "keep" [$node0 get mm:mset:k2]
        }

        test {INFO replication exposes multi-master scaffold state} {
            wait_for_condition 100 50 {
                [s 0 configured_upstreams] == 1 &&
                [s 0 upstream_runtime_entries] == 1 &&
                [s 0 active_upstream_runtime_links] == 1
            } else {
                fail "replication runtime scaffolding did not become active on node1"
            }

            assert_equal 1 [s -1 active_replica]
            assert_equal 1 [s -1 multi_master]
            assert_equal 0 [s -1 configured_upstreams]
            assert_equal 1 [s 0 configured_upstreams]
            assert_equal 0 [s -1 upstream_runtime_entries]
            assert_equal 1 [s 0 upstream_runtime_entries]
            assert_equal 1 [s 0 active_upstream_runtime_links]
            assert {[s 0 upstream_runtime_replay_tx_frames] >= 1}
            assert {[s 0 upstream_runtime_replay_backlog] >= 0}
            assert {[s -1 mvcc_clock] >= 1}
            assert {[s -1 mvcc_key_clock_entries] >= 1}
            assert {[s -1 rreplay_dedupe_entries] >= 1}
            assert {[s -1 mvcc_rdb_clock_max_entries] >= 0}
            assert {[s -1 mvcc_rdb_clock_entries_dropped_last_save] >= 0}
        }

        test {RDB persists RREPLAY dedupe metadata} {
            set dedupe_before [s -1 rreplay_dedupe_entries]
            assert {$dedupe_before >= 1}

            $node0 save
            restart_server -1 true false

            set node0 [srv -1 client]
            set node0_host [srv -1 host]
            set node0_port [srv -1 port]
            set node1 [srv 0 client]
            wait_for_condition 100 100 {
                [s -1 loading] eq {0}
            } else {
                fail "Primary restart after RDB save did not finish loading"
            }

            assert {[s -1 rreplay_dedupe_entries] >= 1}
            $node1 replicaof $node0_host $node0_port
            wait_for_condition 100 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replica link was not re-established after primary restart"
            }
        }

        test {RDB persists configured upstream metadata} {
            set replay_tx_before [s 0 upstream_runtime_replay_tx_frames]
            set replay_ack_before [s 0 upstream_runtime_replay_ack_frames]
            $node1 set mm:mvcc-persist seed
            set mvcc_payload [$node1 dump mm:mvcc-persist]
            $node1 mvccrestore mm:mvcc-persist 0 $mvcc_payload 200 replace
            $node1 save
            restart_server 0 true false

            set node1 [srv 0 client]
            wait_for_condition 100 100 {
                [s 0 loading] eq {0}
            } else {
                fail "Node restart after RDB save did not finish loading"
            }
            $node1 config set active-replica yes
            $node1 config set multi-master yes
            $node1 config set replica-read-only no

            assert_equal 1 [s 0 configured_upstreams]
            wait_for_condition 400 100 {
                [s 0 master_host] eq $node0_host &&
                [s 0 master_port] == $node0_port &&
                [s 0 master_link_status] eq {up}
            } else {}
            set restored_master_host [s 0 master_host]
            set restored_master_port [s 0 master_port]
            set restored_master_state [s 0 master_link_status]
            if {$restored_master_host ne $node0_host ||
                $restored_master_port != $node0_port ||
                $restored_master_state ne "up"} {
                $node1 replicaof $node0_host $node0_port
            }
            wait_for_condition 200 100 {
                [s 0 master_host] eq $node0_host &&
                [s 0 master_port] == $node0_port &&
                [s 0 master_link_status] eq {up}
            } else {
                fail "Configured upstream was restored but active primary link was not re-established after restart"
            }

            $node0 set mm:rdb-reconnect from-node0
            wait_for_condition 100 100 {
                [$node1 get mm:rdb-reconnect] eq {from-node0}
            } else {
                fail "Replica did not receive upstream write after restart"
            }

            $node1 set mm:rdb-reconnect from-node1
            wait_for_condition 100 100 {
                [$node0 get mm:rdb-reconnect] eq {from-node1}
            } else {
                fail "Replica write was not forwarded upstream after restart"
            }

            assert {[s 0 upstream_runtime_replay_tx_frames] >= $replay_tx_before}
            assert {[s 0 upstream_runtime_replay_ack_frames] >= $replay_ack_before}
            $node1 mvccrestore mm:mvcc-persist 0 $mvcc_payload 150 replace
            assert_equal "seed" [$node1 get mm:mvcc-persist]
        }

        test {RDB MVCC cap persists newest key clocks first} {
            $node1 replicaof $node0_host $node0_port
            wait_for_condition 100 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replica link was not established before MVCC cap test"
            }

            $node0 config set mvcc-rdb-clock-max-entries 5
            for {set i 1} {$i <= 8} {incr i} {
                set key "mm:mvcc-cap:$i"
                $node1 set $key "seed-$i"
                wait_for_condition 100 50 {
                    [$node0 get $key] eq "seed-$i"
                } else {
                    fail "seed write for $key did not reach node0"
                }
                set old_payload($i) [$node0 dump $key]
                after 1
                $node1 set $key "final-$i"
                wait_for_condition 100 50 {
                    [$node0 get $key] eq "final-$i"
                } else {
                    fail "final write for $key did not reach node0"
                }
                after 1
            }

            $node0 save
            assert_equal 5 [s -1 mvcc_rdb_clock_max_entries]
            assert {[s -1 mvcc_rdb_clock_entries_dropped_last_save] >= 3}

            restart_server -1 true false
            set node0 [srv -1 client]
            set node0_host [srv -1 host]
            set node0_port [srv -1 port]
            set node1 [srv 0 client]
            wait_for_condition 100 100 {
                [s -1 loading] eq {0}
            } else {
                fail "Primary restart after MVCC cap save did not finish loading"
            }

            $node1 replicaof $node0_host $node0_port
            wait_for_condition 100 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replica link was not re-established after MVCC cap restart"
            }

            for {set i 1} {$i <= 8} {incr i} {
                set key "mm:mvcc-cap:$i"
                $node0 mvccrestore $key 0 $old_payload($i) 1 replace
            }

            for {set i 1} {$i <= 3} {incr i} {
                assert_equal "seed-$i" [$node0 get "mm:mvcc-cap:$i"]
            }
            for {set i 4} {$i <= 8} {incr i} {
                assert_equal "final-$i" [$node0 get "mm:mvcc-cap:$i"]
            }
        }
    }
}
