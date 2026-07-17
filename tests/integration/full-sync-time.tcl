start_server {tags {"repl"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]

    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        test {Assert master_last_full_sync_duration_ms is empty on master} {
            set val [status $master master_last_full_sync_duration_ms]
            assert {$val eq ""}
        }

        test {Assert master_last_full_sync_duration_ms is empty on replica initially} {
            set val [status $replica master_last_full_sync_duration_ms]
            assert {$val eq ""}
        }

        test {Turn replica into a replica of master} {
            $replica replicaof $master_host $master_port
            wait_for_condition 50 100 {
                [status $replica master_link_status] eq "up"
            } else {
                fail "Replica could not connect to master"
            }
        }

        test {Assert master_last_full_sync_duration_ms is reported and valid} {
            set val [status $replica master_last_full_sync_duration_ms]
            assert {$val ne ""}
            assert {[string is integer -strict $val]}
            assert {$val >= 0}
        }

        test {Turn replica into master and assert duration is empty} {
            $replica replicaof no one
            wait_for_condition 50 100 {
                [lindex [$replica role] 0] eq "master"
            } else {
                fail "Replica failed to become master"
            }
            set val [status $replica master_last_full_sync_duration_ms]
            assert {$val eq ""}
        }

        test {Turn replica back to replica of master} {
            $replica replicaof $master_host $master_port
            wait_for_condition 50 100 {
                [status $replica master_link_status] eq "up"
            } else {
                fail "Replica could not reconnect to master"
            }
        }

        test {Assert master_last_full_sync_duration_ms is still reported and valid} {
            set val [status $replica master_last_full_sync_duration_ms]
            assert {$val ne ""}
            assert {[string is integer -strict $val]}
            assert {$val >= 0}
        }
    }
}

start_server {tags {"repl"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]

    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        test {Dual-channel full sync: Turn replica into a replica of master and check metric} {
            # Enable dual-channel configs
            $master config set repl-diskless-sync yes
            $master config set repl-diskless-sync-delay 0
            $replica config set dual-channel-replication-enabled yes
            $replica config set repl-diskless-load swapdb

            $replica replicaof $master_host $master_port
            wait_for_condition 50 100 {
                [status $replica master_link_status] eq "up"
            } else {
                fail "Replica could not connect to master"
            }

            set val [status $replica master_last_full_sync_duration_ms]
            assert {$val ne ""}
            assert {[string is integer -strict $val]}
            assert {$val >= 0}
        }
    }
}
