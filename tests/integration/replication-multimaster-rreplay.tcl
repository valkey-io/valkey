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

        test {INFO replication exposes multi-master scaffold state} {
            assert_equal 1 [s -1 active_replica]
            assert_equal 1 [s -1 multi_master]
            assert_equal 0 [s -1 configured_upstreams]
            assert_equal 1 [s 0 configured_upstreams]
            assert {[s -1 mvcc_clock] >= 1}
            assert {[s -1 mvcc_key_clock_entries] >= 1}
            assert {[s -1 rreplay_dedupe_entries] >= 1}
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

            assert_equal 1 [s 0 configured_upstreams]
            $node1 mvccrestore mm:mvcc-persist 0 $mvcc_payload 150 replace
            assert_equal "seed" [$node1 get mm:mvcc-persist]
        }
    }
}
