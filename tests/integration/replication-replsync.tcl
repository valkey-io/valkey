start_server {tags {repl} overrides {save ""}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    test {REPLSYNC is rejected on a primary} {
        assert_error {*REPLSYNC is only valid on a replica*} {$primary replsync pause}
        assert_error {*REPLSYNC is only valid on a replica*} {$primary replsync resume}
    }

    $primary config set repl-diskless-sync yes
    $primary config set repl-diskless-sync-delay 0
    $primary config set rdb-key-save-delay 1000
    $primary debug populate 2000 replsync 100

    start_server {tags {repl} overrides {save ""}} {
        set replica [srv 0 client]
        $replica replicaof $primary_host $primary_port

        wait_for_condition 200 50 {
            [status $replica master_sync_in_progress] eq 1
        } else {
            fail "Replica did not start a full synchronization"
        }

        test {REPLSYNC PAUSE aborts and holds a full synchronization} {
            assert_equal OK [$replica replsync pause]
            assert_equal OK [$replica replsync pause]
            wait_for_condition 100 20 {
                [status $replica master_sync_paused] eq 1 &&
                [lindex [$replica role] 3] eq "paused"
            } else {
                fail "Replica did not enter paused synchronization state"
            }

            set sync_full [status $primary sync_full]
            after 1200
            assert_equal $sync_full [status $primary sync_full]
            assert_equal down [status $replica master_link_status]
        }

        test {REPLSYNC RESUME reconnects and completes synchronization} {
            $primary config set rdb-key-save-delay 0
            assert_equal OK [$replica replsync resume]
            assert_equal OK [$replica replsync resume]
            wait_for_sync $replica 200 50
            assert_equal 0 [status $replica master_sync_paused]
            assert_equal connected [lindex [$replica role] 3]
        }

        test {REPLSYNC PAUSE does not interrupt an online replication stream} {
            assert_error {*cannot pause an online replication link*} {$replica replsync pause}
        }
    }
}

start_server {tags {repl} overrides {save ""}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    $primary config set repl-diskless-sync no
    $primary config set rdbcompression no
    $primary debug populate 5000 replsync-disk 1000

    start_server {tags {repl} overrides {save "" repl-diskless-load disabled key-load-delay 500 loading-process-events-interval-bytes 1024}} {
        set replica [srv 0 client]
        $replica replicaof $primary_host $primary_port

        wait_for_log_messages 0 {"*Loading DB in memory*"} 0 500 10

        test {REPLSYNC PAUSE safely aborts active disk-based RDB loading} {
            assert_equal OK [$replica replsync pause]
            wait_for_condition 200 20 {
                [status $replica master_sync_paused] eq 1 &&
                [lindex [$replica role] 3] eq "paused" &&
                [status $replica loading] eq 0
            } else {
                fail "Replica did not safely unwind and pause disk-based loading"
            }

            set sync_full [status $primary sync_full]
            after 1200
            assert_equal $sync_full [status $primary sync_full]
        }

        test {REPLSYNC RESUME succeeds after aborting disk-based loading} {
            $replica config set key-load-delay 0
            assert_equal OK [$replica replsync resume]
            wait_for_sync $replica 300 50
        }
    }
}

start_server {tags {repl} overrides {save ""}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    $primary config set repl-diskless-sync yes
    $primary config set repl-diskless-sync-delay 0
    $primary config set rdbcompression no
    $primary debug populate 5000 replsync-load 1000

    start_server {tags {repl} overrides {save "" repl-diskless-load flush-before-load key-load-delay 500 loading-process-events-interval-bytes 1024}} {
        set replica [srv 0 client]
        set replica_log [srv 0 stdout]
        $replica replicaof $primary_host $primary_port

        set log_result [wait_for_log_messages 0 {"*Loading DB in memory*"} 0 500 10]
        set loglines [lindex $log_result 1]

        test {REPLSYNC PAUSE safely aborts active diskless RDB loading} {
            assert_equal OK [$replica replsync pause]
            wait_for_condition 200 20 {
                [status $replica master_sync_paused] eq 1 &&
                [lindex [$replica role] 3] eq "paused" &&
                [status $replica loading] eq 0
            } else {
                fail "Replica did not safely unwind and pause diskless loading"
            }
            wait_for_log_messages 0 {"*Failed trying to load the PRIMARY synchronization DB from socket*"} $loglines 500 10

            set sync_full [status $primary sync_full]
            after 1200
            assert_equal $sync_full [status $primary sync_full]
        }

        test {REPLSYNC RESUME succeeds after aborting diskless loading} {
            $replica config set key-load-delay 0
            assert_equal OK [$replica replsync resume]
            wait_for_sync $replica 300 50
        }
    }
}
