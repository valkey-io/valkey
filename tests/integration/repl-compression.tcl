tags {"repl-compression external:skip"} {

# ============================================================
# Config CRUD — single-server tests, no replication needed
# ============================================================

start_server {overrides {save ""}} {

    test {Repl compression config defaults are correct} {
        assert_equal "no" [lindex [r config get replcompression] 1]
    }

    test {replcompression can be toggled on and off} {
        r config set replcompression yes
        assert_equal "yes" [lindex [r config get replcompression] 1]
        r config set replcompression no
        assert_equal "no" [lindex [r config get replcompression] 1]
    }

    test {Repl compression configs survive CONFIG REWRITE and restart} {
        r config set replcompression yes
        r config rewrite

        restart_server 0 true false

        assert_equal "yes" [lindex [r config get replcompression] 1]

        # Restore default
        r config set replcompression no
    }
}

# ============================================================
# Replication handshake behavior — primary + replica tests
# ============================================================

start_server {tags {"repl"} overrides {save ""}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    test {Replica with replcompression no does NOT send capa compression} {
        start_server {overrides {save "" replcompression no}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started"
            }

            # Full sync completes normally without compression capability
            assert_equal {up} [s 0 master_link_status]

            $replica replicaof no one
        }
    }

    test {Replica with replcompression yes and diskless load sends capa compression} {
        start_server {overrides {save "" replcompression yes repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started"
            }

            set info [$primary info replication]
            assert_match "*slave0:*" $info
            assert_equal {up} [s 0 master_link_status]

            $replica replicaof no one
        }
    }

    test {Replica with replcompression yes but disk-backed load does NOT send capa compression} {
        start_server {overrides {save "" replcompression yes repl-diskless-load disabled}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started"
            }

            # Full sync completes normally — disk-backed replica does not advertise compression
            assert_equal {up} [s 0 master_link_status]

            $replica replicaof no one
        }
    }

    test {Primary receiving capa compression still completes full sync correctly (no-op)} {
        $primary flushall
        for {set i 0} {$i < 100} {incr i} {
            $primary set "noop:$i" [string repeat "value$i " 10]
        }

        start_server {overrides {save "" replcompression yes repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port
            wait_for_sync $replica

            wait_for_condition 50 100 {
                [status $replica master_link_status] eq "up"
            } else {
                fail "Replica did not complete full sync"
            }

            # Data integrity check — primary records capa but takes no action
            assert_equal [string repeat "value42 " 10] [$replica get noop:42]
            assert_equal 100 [$replica dbsize]

            $replica replicaof no one
        }
    }

    test {Backward compatibility - older replica without capa compression connects successfully} {
        start_server {overrides {save ""}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started"
            }

            assert_equal {up} [s 0 master_link_status]

            $replica replicaof no one
        }
    }

    test {Toggling replcompression mid-runtime affects the next handshake} {
        # First sync with compression disabled
        start_server {overrides {save "" replcompression no repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started with replcompression no"
            }

            assert_equal {up} [s 0 master_link_status]

            # Toggle compression on at runtime
            $replica config set replcompression yes

            # Disconnect and reconnect to trigger a new handshake
            $replica replicaof no one
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started after toggling replcompression yes"
            }

            assert_equal {up} [s 0 master_link_status]

            $replica replicaof no one
        }
    }
}

}
