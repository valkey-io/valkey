start_server {tags {"repl external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]

    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        test {Assert last_successful_sync_duration_ms is empty on master} {
            set val [status $master last_successful_sync_duration_ms]
            assert {$val eq ""}
        }

        test {Assert last_successful_sync_duration_ms is empty on replica initially} {
            set val [status $replica last_successful_sync_duration_ms]
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

        test {Assert last_successful_sync_duration_ms is reported and valid} {
            set val [status $replica last_successful_sync_duration_ms]
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
            set val [status $replica last_successful_sync_duration_ms]
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

        test {Assert last_successful_sync_duration_ms is still reported and valid} {
            set val [status $replica last_successful_sync_duration_ms]
            assert {$val ne ""}
            assert {[string is integer -strict $val]}
            assert {$val >= 0}
        }

        test {Assert last_successful_sync_duration_ms is NOT reset when primary changes directly} {
            set val [status $replica last_successful_sync_duration_ms]
            assert {$val ne ""}
            assert {$val >= 0}
            set old_val $val

            # Change primary to a non-existent one
            $replica replicaof 127.0.0.1 9999

            # The metric should NOT be reset, it should keep the old value
            set val [status $replica last_successful_sync_duration_ms]
            assert {$val == $old_val}

            # Clean up
            $replica replicaof no one
        }

        test {Assert last_successful_sync_duration_ms is reset when transitioning replica -> master -> replica (failed sync)} {
            # Sync with master first to get a valid duration
            $replica replicaof $master_host $master_port
            wait_for_condition 50 100 {
                [status $replica master_link_status] eq "up"
            } else {
                fail "Replica could not reconnect to master"
            }
            assert {[status $replica last_successful_sync_duration_ms] >= 0}

            # Turn replica into master
            $replica replicaof no one
            wait_for_condition 50 100 {
                [lindex [$replica role] 0] eq "master"
            } else {
                fail "Replica failed to become master"
            }

            # Turn replica into replica of non-existent master
            $replica replicaof 127.0.0.1 9999

            # The metric should be -1
            set val [status $replica last_successful_sync_duration_ms]
            assert {$val == -1}

            # Clean up
            $replica replicaof no one
        }
    }
}

start_server {tags {"repl external:skip"}} {
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
            $master config set dual-channel-replication-enabled yes
            $replica config set dual-channel-replication-enabled yes
            $replica config set repl-diskless-load swapdb

            $replica replicaof $master_host $master_port
            wait_for_condition 50 100 {
                [status $replica master_link_status] eq "up"
            } else {
                fail "Replica could not connect to master"
            }

            set val [status $replica last_successful_sync_duration_ms]
            assert {$val ne ""}
            assert {[string is integer -strict $val]}
            assert {$val >= 0}
        }
    }
}
