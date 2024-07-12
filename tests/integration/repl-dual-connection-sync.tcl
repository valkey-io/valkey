proc log_file_matches {log pattern} {
    set fp [open $log r]
    set content [read $fp]
    close $fp
    string match $pattern $content
}

proc start_bg_server_sleep {host port sec} {
    set tclsh [info nameofexecutable]
    exec $tclsh tests/helpers/bg_server_sleep.tcl $host $port $sec &
}

proc stop_bg_server_sleep {handle} {
    catch {exec /bin/kill -9 $handle}
}

proc get_client_id_by_last_cmd {r cmd} {
    set client_list [$r client list]
    set client_id ""
    set lines [split $client_list "\n"]
    foreach line $lines {
        if {[string match *cmd=$cmd* $line]} {
            set parts [split $line " "]
            foreach part $parts {
                if {[string match id=* $part]} {
                    set client_id [lindex [split $part "="] 1]
                    return $client_id
                }
            }
        }
    }
    return $client_id
}

start_server {tags {"repl dual-connection external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_log [srv 0 stdout]
    start_server {} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        # Configure the primary in order to hang waiting for the BGSAVE
        # operation, so that the replica remains in the handshake state.
        $primary config set repl-diskless-sync yes
        $primary config set repl-diskless-sync-delay 1000
        $primary config set dual-conn-sync-enabled yes

        # Start the replication process...
        $replica config set dual-conn-sync-enabled yes
        $replica replicaof $primary_host $primary_port

        test "Test dual-conn-sync-enabled replica enters handshake" {
            wait_for_condition 50 1000 {
                [string match *handshake* [$replica role]]
            } else {
                fail "Replica does not enter handshake state"
            }
        }

        test "Test dual-conn-sync-enabled enters wait_bgsave" {
            wait_for_condition 50 1000 {
                [string match *state=wait_bgsave* [$primary info replication]]
            } else {
                fail "Replica does not enter wait_bgsave state"
            }
        }

        $primary config set repl-diskless-sync-delay 0

        test "Test dual-conn-sync-enabled replica is able to sync" {
            verify_replica_online $primary 0 500
            wait_for_condition 50 1000 {
                [string match *connected_slaves:1* [$primary info]]
            } else {
                fail "Replica rdb connection is still open"
            }
            set offset [status $primary master_repl_offset]
            wait_for_condition 500 100 {
                [string match "*slave0:*,offset=$offset,*" [$primary info replication]] &&
                $offset == [status $replica master_repl_offset]
            } else {
                fail "Replicas and primary offsets were unable to match."
            }
        }
    }
}

start_server {tags {"repl dual-connection external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_log [srv 0 stdout]
    start_server {} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        $primary config set rdb-key-save-delay 200
        $primary config set dual-conn-sync-enabled yes
        $replica config set dual-conn-sync-enabled yes
        $replica config set repl-diskless-sync no

        populate 1000 primary 10000
        set load_handle1 [start_one_key_write_load $primary_host $primary_port 100 "mykey1"]
        set load_handle2 [start_one_key_write_load $primary_host $primary_port 100 "mykey2"]
        set load_handle3 [start_one_key_write_load $primary_host $primary_port 100 "mykey3"]
        
        # wait for load handlers to start
        wait_for_condition 50 1000 {
            ([$primary get "mykey1"] != "") &&
            ([$primary get "mykey2"] != "") &&
            ([$primary get "mykey3"] != "")
        } else {
            fail "Can't set new keys"
        }

        set before_used [s 0 used_memory]

        test "Primary memory usage does not increase during dual-connection sync" {        
            $replica replicaof $primary_host $primary_port

            # Verify used_memory stays low through all the sync
            set max_retry 500
            while {$max_retry} {
                # Verify memory
                set used_memory [s 0 used_memory]
                assert {$used_memory-$before_used <= 1.5*10^6}; # ~1/3 of the space
                # Check replica state
                set primary_info [$primary info]
                set replica_info [$replica info]
                if {[string match *slave0:*state=online* $primary_info] &&
                    [string match *master_link_status:up* $replica_info]} {
                    break
                } else {
                    incr max_retry -1
                    after 10
                }
            }
            if {$max_retry == 0} {
                error "assertion:Replica not in sync after 5 seconds"
            }
        }
        stop_write_load $load_handle1
        stop_write_load $load_handle2
        stop_write_load $load_handle3

        test "Steady state after dual conn sync" {
            wait_for_condition 50 1000 {
                ([$replica get "mykey1"] eq [$primary get mykey1]) &&
                ([$replica get "mykey2"] eq [$primary get mykey2]) &&
                ([$replica get "mykey3"] eq [$primary get mykey3])
            } else {
                fail "Can't set new keys"
            }
        }

        test "Dual connection sync doesn't impair subsequent normal syncs" {
            $replica replicaof no one
            $replica config set dual-conn-sync-enabled no
            $primary set newkey newval

            set sync_full [s 0 sync_full]
            set sync_partial [s 0 sync_partial_ok]

            $replica replicaof $primary_host $primary_port
            verify_replica_online $primary 0 500
            # Verify replica used  normal sync this time
            assert_equal [expr $sync_full + 1] [s 0 sync_full]
            assert_equal [expr $sync_partial] [s 0 sync_partial_ok]
            assert [string match *connected_slaves:1* [$primary info]]
        }
    }
}

start_server {tags {"repl dual-connection external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_log [srv 0 stdout]
    start_server {} {
        foreach enable {yes no} {
            set primary [srv 0 client]
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]

            $primary config set repl-diskless-sync yes
            # Set primary shared replication buffer size to a bit more then the size of 
            # a replication buffer block.
            $primary config set client-output-buffer-limit "replica 1100k 0 0"
            $primary config set dual-conn-sync-enabled $enable
            $replica config set dual-conn-sync-enabled $enable

            test "Toggle dual-conn-sync-enabled: $enable start" {    
                populate 1000 primary 10000
                set prev_sync_full [s 0 sync_full]
                set prev_sync_partial [s 0 sync_partial_ok]

                $replica replicaof $primary_host $primary_port
                verify_replica_online $primary 0 500
                wait_for_sync $replica


                set cur_sync_full [s 0 sync_full]
                set cur_sync_partial [s 0 sync_partial_ok]
                if {$enable == "yes"} {
                    # Verify that dual connection sync was used
                    assert {$cur_sync_full == [expr $prev_sync_full + 1]}
                    assert {$cur_sync_partial == [expr $prev_sync_partial + 1]}
                } else {
                    # Verify that normal sync was used
                    assert {[s 0 sync_full] == [expr $prev_sync_full + 1]}
                    assert {[s 0 sync_partial_ok] == $prev_sync_partial}
                }

                $replica replicaof no one
                if {$enable == "yes"} {
                    # disable dual conn sync
                    $replica config set dual-conn-sync-enabled no
                    $primary config set dual-conn-sync-enabled no
                } else {
                    $replica config set dual-conn-sync-enabled yes
                    $primary config set dual-conn-sync-enabled yes
                }

                # Force replica to full sync next time
                populate 1000 primary 10000
                set prev_sync_full [s 0 sync_full]
                set prev_sync_partial [s 0 sync_partial_ok]

                $replica replicaof $primary_host $primary_port
                verify_replica_online $primary 0 500
                wait_for_sync $replica

                set cur_sync_full [s 0 sync_full]
                set cur_sync_partial [s 0 sync_partial_ok]
                if {$enable == "yes"} {
                    # Verify that normal sync was used
                    assert {$cur_sync_full == [expr $prev_sync_full + 1]}
                    assert {$cur_sync_partial == $prev_sync_partial}
                } else {
                    # Verify that dual connection sync was used
                    assert {$cur_sync_full == [expr $prev_sync_full + 1]}
                    assert {$cur_sync_partial == [expr $prev_sync_partial + 1]}
                }
                $replica replicaof no one
            }
        }
    }
}

start_server {tags {"repl dual-connection external:skip"}} {
    set replica1 [srv 0 client]
    set replica1_host [srv 0 host]
    set replica1_port [srv 0 port]
    set replica1_log [srv 0 stdout]
    start_server {} {
        set replica2 [srv 0 client]
        set replica2_host [srv 0 host]
        set replica2_port [srv 0 port]
        set replica2_log [srv 0 stdout]
        start_server {} {
            set primary [srv 0 client]
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]
            set loglines [count_log_lines -1]

            populate 10000 primary 10000
            $primary set key1 val1

            $primary config set repl-diskless-sync yes
            $primary config set repl-diskless-sync-delay 5; # allow both replicas to ask for sync
            $primary config set dual-conn-sync-enabled yes

            $replica1 config set dual-conn-sync-enabled yes
            $replica2 config set dual-conn-sync-enabled yes
            $replica1 config set repl-diskless-sync no
            $replica2 config set repl-diskless-sync no
            $replica1 config set loglevel debug
            $replica2 config set loglevel debug

            test "Dual-conn-sync with multiple replicas" {
                $replica1 replicaof $primary_host $primary_port
                $replica2 replicaof $primary_host $primary_port

                wait_for_value_to_propegate_to_replica $primary $replica1 "key1"
                wait_for_value_to_propegate_to_replica $primary $replica2 "key1"

                wait_for_condition 100 100 {
                    [s 0 total_forks] eq "1" 
                } else {
                    fail "Primary <-> Replica didn't start the full sync"
                }

                verify_replica_online $primary 0 500
                verify_replica_online $primary 1 500
                wait_for_condition 50 1000 {
                    [status $replica1 master_link_status] == "up"
                } else {
                    fail "Replica is not synced"
                }           
            }

            $replica1 replicaof no one
            $replica2 replicaof no one

            $replica1 config set dual-conn-sync-enabled yes
            $replica2 config set dual-conn-sync-enabled no

            $primary set key2 val2

            test "Test diverse replica sync: dual-conn on/off" {
                $replica1 replicaof $primary_host $primary_port
                $replica2 replicaof $primary_host $primary_port

                wait_for_value_to_propegate_to_replica $primary $replica1 "key2"
                wait_for_value_to_propegate_to_replica $primary $replica2 "key2"

                verify_replica_online $primary 0 500
                verify_replica_online $primary 1 500
                wait_for_condition 50 1000 {
                    [status $replica1 master_link_status] == "up"
                } else {
                    fail "Replica is not synced"
                }
            }

            $replica1 replicaof no one
            $primary set key4 val4            
            
            test "Test replica's buffer limit reached" {
                $primary config set repl-diskless-sync-delay 0
                $primary config set rdb-key-save-delay 500
                # At this point we have about 10k keys in the db, 
                # We expect that the next full sync will take 5 seconds (10k*500)ms
                # It will give us enough time to fill the replica buffer.
                $replica1 config set dual-conn-sync-enabled yes
                $replica1 config set client-output-buffer-limit "replica 16383 16383 0"
                $replica1 config set loglevel debug

                $replica1 replicaof $primary_host $primary_port
                # Wait for replica to establish psync using main connection
                wait_for_condition 500 1000 {
                    [string match "*slave*,state=bg_transfer*,type=main-conn*" [$primary info replication]]
                } else {
                    fail "replica didn't start sync session in time"
                }  

                populate 10000 primary 10000
                # Wait for replica's buffer limit reached
                wait_for_condition 50 1000 {
                    [log_file_matches $replica1_log "*Replication buffer limit reached, stopping buffering*"]
                } else {
                    fail "Replica buffer should fill"
                }
                assert {[s -2 replicas_replication_buffer_size] <= 16385*2}

                # Wait for sync to succeed 
                wait_for_value_to_propegate_to_replica $primary $replica1 "key4"
                wait_for_condition 50 1000 {
                    [status $replica1 master_link_status] == "up"
                } else {
                    fail "Replica is not synced"
                }
            }

            $replica1 replicaof no one
            $replica1 config set client-output-buffer-limit "replica 256mb 256mb 0"; # remove repl buffer limitation

            $primary set key5 val5
            
            test "Dual-conn-sync fails when primary diskless disabled" {
                set cur_psync [status $primary sync_partial_ok]
                $primary config set repl-diskless-sync no

                $replica1 config set dual-conn-sync-enabled yes
                $replica1 replicaof $primary_host $primary_port

                # Wait for mitigation and resync
                wait_for_value_to_propegate_to_replica $primary $replica1 "key5"

                wait_for_condition 50 1000 {
                    [status $replica1 master_link_status] == "up"
                } else {
                    fail "Replica is not synced"
                }

                # Verify that we did not use dual-connection sync
                assert {[status $primary sync_partial_ok] == $cur_psync}
            }
        }
    }
}

start_server {tags {"repl dual-connection external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_log [srv 0 stdout]
    start_server {} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        set loglines [count_log_lines -1]
        # Create small enough db to be loaded before replica establish psync connection
        $primary set key1 val1

        $primary config set repl-diskless-sync yes
        $primary debug sleep-after-fork-seconds 5;# Stop primary after fork for 5 seconds
        $primary config set dual-conn-sync-enabled yes

        $replica config set dual-conn-sync-enabled yes
        $replica config set loglevel debug

        test "Test dual-connection sync- psync established after rdb load" {
            $replica replicaof $primary_host $primary_port

            verify_replica_online $primary 0 500
            wait_for_condition 50 1000 {
                [status $replica master_link_status] == "up"
            } else {
                fail "Replica is not synced"
            }
            wait_for_value_to_propegate_to_replica $primary $replica "key1"
            # Confirm the occurrence of a race condition.
            set res [wait_for_log_messages -1 {"*Dual connection sync - psync established after rdb load*"} $loglines 2000 1]
            set loglines [lindex $res 1]
            incr $loglines                
        }
    }
}

start_server {tags {"repl dual-connection external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_log [srv 0 stdout]
    start_server {} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        set backlog_size [expr {10 ** 5}]
        set loglines [count_log_lines -1]

        $primary config set repl-diskless-sync yes
        $primary config set dual-conn-sync-enabled yes
        $primary config set repl-backlog-size $backlog_size
        $primary config set loglevel debug
        $primary config set repl-timeout 10
        $primary config set rdb-key-save-delay 200
        populate 10000 primary 10000
        
        set load_handle1 [start_one_key_write_load $primary_host $primary_port 100 "mykey1"]
        set load_handle2 [start_one_key_write_load $primary_host $primary_port 100 "mykey2"]
        set load_handle3 [start_one_key_write_load $primary_host $primary_port 100 "mykey3"]

        $replica config set dual-conn-sync-enabled yes
        $replica config set loglevel debug
        $replica config set repl-timeout 10
        # Stop replica after primary fork for 5 seconds
        $replica debug sleep-after-fork-seconds 5

        test "Dual-conn-sync: Primary COB growth with inactive replica" {
            $replica replicaof $primary_host $primary_port
            # Verify repl backlog can grow
            wait_for_condition 1000 10 {
                [s 0 mem_total_replication_buffers] > [expr {2 * $backlog_size}]
            } else {
                fail "Primary should allow backlog to grow beyond its limits during dual-connection sync handshake"
            }

            verify_replica_online $primary 0 500
            wait_for_condition 50 1000 {
                [status $replica master_link_status] == "up"
            } else {
                fail "Replica is not synced"
            }
        }

        stop_write_load $load_handle1
        stop_write_load $load_handle2
        stop_write_load $load_handle3

    }
}

start_server {tags {"repl dual-connection external:skip"}} {
    set replica1 [srv 0 client]
    set replica1_host [srv 0 host]
    set replica1_port [srv 0 port]
    set replica1_log [srv 0 stdout]
    start_server {} {
        set replica2 [srv 0 client]
        set replica2_host [srv 0 host]
        set replica2_port [srv 0 port]
        set replica2_log [srv 0 stdout]
        start_server {} {
            set primary [srv 0 client]
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]
            set backlog_size [expr {10 ** 6}]
            set loglines [count_log_lines -1]

            $primary config set repl-diskless-sync yes
            $primary config set dual-conn-sync-enabled yes
            $primary config set repl-backlog-size $backlog_size
            $primary config set loglevel debug
            $primary config set repl-timeout 10
            $primary config set rdb-key-save-delay 10
            populate 1024 primary 16
            
            set load_handle0 [start_write_load $primary_host $primary_port 20]

            $replica1 config set dual-conn-sync-enabled yes
            $replica2 config set dual-conn-sync-enabled yes
            $replica1 config set loglevel debug
            $replica2 config set loglevel debug
            $replica1 config set repl-timeout 10
            $replica2 config set repl-timeout 10

            # Stop replica after primary fork for 2 seconds
            $replica1 debug sleep-after-fork-seconds 2
            $replica2 debug sleep-after-fork-seconds 2
            test "Test dual-conn-sync: primary tracking replica backlog refcount - start with empty backlog" {
                $replica1 replicaof $primary_host $primary_port
                set res [wait_for_log_messages 0 {"*Add rdb replica * no repl-backlog to track*"} $loglines 2000 1]
                set res [wait_for_log_messages 0 {"*Attach rdb replica*"} $loglines 2000 1]
                set loglines [lindex $res 1]
                incr $loglines 
                verify_replica_online $primary 0 700
                wait_for_condition 50 1000 {
                    [status $replica1 master_link_status] == "up"
                } else {
                    fail "Replica is not synced"
                }
                $replica1 replicaof no one
                assert [string match *replicas_waiting_psync:0* [$primary info replication]]
            }

            test "Test dual-conn-sync: primary tracking replica backlog refcount - start with backlog" {
                $replica2 replicaof $primary_host $primary_port
                set res [wait_for_log_messages 0 {"*Add rdb replica * tracking repl-backlog tail*"} $loglines 2000 1]
                set loglines [lindex $res 1]
                incr $loglines 
                verify_replica_online $primary 0 700
                wait_for_condition 50 1000 {
                    [status $replica2 master_link_status] == "up"
                } else {
                    fail "Replica is not synced"
                }
                assert [string match *replicas_waiting_psync:0* [$primary info replication]]
            }

            stop_write_load $load_handle0
        }
    }
}

start_server {tags {"repl dual-connection external:skip"}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    
    $primary config set repl-diskless-sync yes
    $primary config set dual-conn-sync-enabled yes
    $primary config set repl-backlog-size [expr {10 ** 6}]
    $primary config set loglevel debug
    $primary config set repl-timeout 10
    # generate small db
    populate 10 primary 10
    # Stop primary main process after fork for 2 seconds
    $primary debug sleep-after-fork-seconds 2
    $primary debug delay-rdb-client-free-seconds 5

    start_server {} {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]
        set replica_log [srv 0 stdout]
        set loglines [count_log_lines 0]
        
        set load_handle0 [start_write_load $primary_host $primary_port 20]

        $replica config set dual-conn-sync-enabled yes
        $replica config set loglevel debug
        $replica config set repl-timeout 10

        test "Psync established after rdb load - within grace period" {
            $replica replicaof $primary_host $primary_port
            set res [wait_for_log_messages 0 {"*Done loading RDB*"} $loglines 2000 1]
            set loglines [lindex $res 1]
            incr $loglines
            # At this point rdb is loaded but psync hasn't been established yet. 
            # Force the replica to sleep for 3 seconds so the primary main process will wake up, while the replica is unresponsive.
            set sleep_handle [start_bg_server_sleep $replica_host $replica_port 3]
            wait_for_condition 50 100 {
                [string match {*replicas_waiting_psync:1*} [$primary info replication]]
            } else {
                fail "Primary freed RDB client before psync was established"
            }

            verify_replica_online $primary 0 500
            wait_for_condition 50 100 {
                [string match {*replicas_waiting_psync:0*} [$primary info replication]]
            } else {
                fail "Primary did not free repl buf block after psync establishment"
            }
            $replica replicaof no one
        }
        stop_write_load $load_handle0
    }

    start_server {} {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]
        set replica_log [srv 0 stdout]
        set loglines [count_log_lines 0]
        
        set load_handle0 [start_write_load $primary_host $primary_port 20]

        $replica config set dual-conn-sync-enabled yes
        $replica config set loglevel debug
        $replica config set repl-timeout 10

        test "Psync established after RDB load - beyond grace period" {
            $replica replicaof $primary_host $primary_port
            set res [wait_for_log_messages 0 {"*Done loading RDB*"} $loglines 2000 1]
            set loglines [lindex $res 1]
            incr $loglines
            # At this point rdb is loaded but psync hasn't been established yet. 
            # Force the replica to sleep for 8 seconds so the primary main process will wake up, while the replica is unresponsive.
            # We expect the grace time to be over before the replica wake up, so sync will fail.
            set sleep_handle [start_bg_server_sleep $replica_host $replica_port 8]
            wait_for_condition 50 100 {
                [string match {*replicas_waiting_psync:1*} [$primary info replication]]
            } else {
                fail "Primary should wait before freeing repl block"
            }

            # Sync should fail once the replica ask for PSYNC using main connection
            set res [wait_for_log_messages -1 {"*Replica main connection failed to establish PSYNC within the grace period*"} 0 4000 1]

            # Should succeed on retry
            verify_replica_online $primary 0 500
            wait_for_condition 50 100 {
                [string match {*replicas_waiting_psync:0*} [$primary info replication]]
            } else {
                fail "Primary did not free repl buf block after psync establishment"
            }
            $replica replicaof no one
        }
        stop_write_load $load_handle0
    }
}

start_server {tags {"repl dual-connection external:skip"}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set loglines [count_log_lines 0]

    $primary config set repl-diskless-sync yes
    $primary config set dual-conn-sync-enabled yes
    $primary config set client-output-buffer-limit "replica 1100k 0 0"
    $primary config set loglevel debug
    # generate small db
    populate 10 primary 10
    # Stop primary main process after fork for 1 seconds
    $primary debug sleep-after-fork-seconds 2
    start_server {} {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]
        set replica_log [srv 0 stdout]
        
        set load_handle0 [start_write_load $primary_host $primary_port 20]
        set load_handle1 [start_write_load $primary_host $primary_port 20]
        set load_handle2 [start_write_load $primary_host $primary_port 20]

        $replica config set dual-conn-sync-enabled yes
        $replica config set loglevel debug
        $replica config set repl-timeout 10

        test "Test dual-connection primary gets cob overrun before established psync" {
            $replica replicaof $primary_host $primary_port
            wait_for_log_messages 0 {"*Done loading RDB*"} 0 2000 1

            # At this point rdb is loaded but psync hasn't been established yet. 
            # Force the replica to sleep for 5 seconds so the primary main process will wake up while the
            # replica is unresponsive. We expect the main process to fill the COB before the replica wakes.
            set sleep_handle [start_bg_server_sleep $replica_host $replica_port 5]
            wait_for_log_messages -1 {"*Client * closed * for overcoming of output buffer limits.*"} $loglines 2000 1
            wait_for_condition 50 100 {
                [string match {*replicas_waiting_psync:0*} [$primary info replication]]
            } else {
                fail "Primary did not free repl buf block after sync failure"
            }
            set res [wait_for_log_messages -1 {"*Unable to partial resync with replica * for lack of backlog*"} $loglines 20000 1]
            set loglines [lindex $res 1]
        }

        $replica replicaof no one
        $replica debug sleep-after-fork-seconds 2

        $primary debug populate 1000 primary 100000
        # Set primary with a slow rdb generation, so that we can easily intercept loading
        # 10ms per key, with 1000 keys is 10 seconds
        $primary config set rdb-key-save-delay 10000
        $primary debug sleep-after-fork-seconds 0

        test "Test dual-connection primary gets cob overrun during replica rdb load" {
            set cur_client_closed_count [s -1 client_output_buffer_limit_disconnections]
            $replica replicaof $primary_host $primary_port
            wait_for_condition 500 1000 {
                [s -1 client_output_buffer_limit_disconnections] > $cur_client_closed_count
            } else {
                fail "Primary should disconnect replica due to COB overrun"
            }

            wait_for_condition 50 100 {
                [string match {*replicas_waiting_psync:0*} [$primary info replication]]
            } else {
                fail "Primary did not free repl buf block after sync failure"
            }
            set res [wait_for_log_messages -1 {"*Unable to partial resync with replica * for lack of backlog*"} $loglines 20000 1]
            set loglines [lindex $res 0]
        }
        stop_write_load $load_handle0
        stop_write_load $load_handle1
        stop_write_load $load_handle2
    }
}

foreach dualconn {yes no} {
start_server {tags {"repl dual-connection external:skip"}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set loglines [count_log_lines 0]

    $primary config set repl-diskless-sync yes
    $primary config set dual-conn-sync-enabled yes
    $primary config set loglevel debug
    $primary config set repl-diskless-sync-delay 5
    
    # Generating RDB will cost 5s(10000 * 0.0005s)
    $primary debug populate 10000 primary 1
    $primary config set rdb-key-save-delay 500

    $primary config set dual-conn-sync-enabled $dualconn

    start_server {} {
        set replica1 [srv 0 client]
        $replica1 config set dual-conn-sync-enabled $dualconn
        $replica1 config set loglevel debug
        start_server {} {
            set replica2 [srv 0 client]
            $replica2 config set dual-conn-sync-enabled $dualconn
            $replica2 config set loglevel debug
            $replica2 config set repl-timeout 60

            set load_handle [start_one_key_write_load $primary_host $primary_port 100 "mykey1"]
            test "Sync should continue if not all slaves dropped dual-connection $dualconn" {
                $replica1 replicaof $primary_host $primary_port
                $replica2 replicaof $primary_host $primary_port

                wait_for_condition 50 1000 {
                    [status $primary rdb_bgsave_in_progress] == 1
                } else {
                    fail "Sync did not start"
                }
                if {$dualconn == "yes"} {
                    # Wait for both replicas main conns to establish psync
                    wait_for_condition 50 1000 {
                        [status $primary sync_partial_ok] == 2
                    } else {
                        fail "Replicas main conns didn't establish psync [status $primary sync_partial_ok]"
                    }
                }

                catch {$replica1 shutdown nosave}
                wait_for_condition 50 2000 {
                    [status $replica2 master_link_status] == "up" &&
                    [status $primary sync_full] == 2 &&
                    (($dualconn == "yes" && [status $primary sync_partial_ok] == 2) || $dualconn == "no")
                } else {
                    fail "Sync session interapted\n
                        sync_full:[status $primary sync_full]\n
                        sync_partial_ok:[status $primary sync_partial_ok]"
                }
            }
            
            $replica2 replicaof no one

            # Generating RDB will cost 500s(1000000 * 0.0001s)
            $primary debug populate 1000000 primary 1
            $primary config set rdb-key-save-delay 100
    
            test "Primary abort sync if all slaves dropped dual-connection $dualconn" {
                set cur_psync [status $primary sync_partial_ok]
                $replica2 replicaof $primary_host $primary_port

                wait_for_condition 50 1000 {
                    [status $primary rdb_bgsave_in_progress] == 1
                } else {
                    fail "Sync did not start"
                }
                if {$dualconn == "yes"} {
                    # Wait for both replicas main conns to establish psync
                    wait_for_condition 50 1000 {
                        [status $primary sync_partial_ok] == $cur_psync + 1
                    } else {
                        fail "Replicas main conns didn't establish psync [status $primary sync_partial_ok]"
                    }
                }

                catch {$replica2 shutdown nosave}
                wait_for_condition 50 1000 {
                    [status $primary rdb_bgsave_in_progress] == 0
                } else {
                    fail "Primary should abort the sync"
                }
            }
            stop_write_load $load_handle
        }
    }
}
}

start_server {tags {"repl dual-connection external:skip"}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set loglines [count_log_lines 0]

    $primary config set repl-diskless-sync yes
    $primary config set dual-conn-sync-enabled yes
    $primary config set loglevel debug
    $primary config set repl-diskless-sync-delay 5; # allow catch failed sync before retry

    # Generating RDB will cost 500s(1000000 * 0.0001s)
    $primary debug populate 1000000 primary 1
    $primary config set rdb-key-save-delay 100
    
    start_server {} {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]
        set replica_log [srv 0 stdout]
        
        set load_handle [start_write_load $primary_host $primary_port 20]

        $replica config set dual-conn-sync-enabled yes
        $replica config set loglevel debug
        $replica config set repl-timeout 10
        test "Test dual-connection replica main connection disconnected" {
            $replica replicaof $primary_host $primary_port
            # Wait for sync session to start
            wait_for_condition 500 1000 {
                [string match "*slave*,state=wait_bgsave*,type=rdb-conn*" [$primary info replication]] &&
                [string match "*slave*,state=bg_transfer*,type=main-conn*" [$primary info replication]] &&
                [s -1 rdb_bgsave_in_progress] eq 1
            } else {
                fail "replica didn't start sync session in time"
            }            

            $primary debug log "killing replica main connection"
            set replica_main_conn_id [get_client_id_by_last_cmd $primary "psync"]
            assert {$replica_main_conn_id != ""}
            $primary client kill id $replica_main_conn_id
            # Wait for primary to abort the sync
            wait_for_condition 50 1000 {
                [string match {*replicas_waiting_psync:0*} [$primary info replication]]
            } else {
                fail "Primary did not free repl buf block after sync failure"
            }
            wait_for_condition 1000 10 {
                [s -1 rdb_last_bgsave_status] eq "err"
            } else {
                fail "bgsave did not stop in time"
            }
        }

        test "Test dual connection slave of no one" {
            $replica replicaof no one
            wait_for_condition 500 1000 {
                [s -1 rdb_bgsave_in_progress] eq 0
            } else {
                fail "Primary should abort sync"
            }
        }

        test "Test dual-connection replica rdb connection disconnected" {
            $replica replicaof $primary_host $primary_port
            # Wait for sync session to start
            wait_for_condition 500 1000 {
                [string match "*slave*,state=wait_bgsave*,type=rdb-conn*" [$primary info replication]] &&
                [string match "*slave*,state=bg_transfer*,type=main-conn*" [$primary info replication]] &&
                [s -1 rdb_bgsave_in_progress] eq 1
            } else {
                fail "replica didn't start sync session in time"
            }            

            $primary debug log "killing replica rdb connection"
            set replica_main_conn_id [get_client_id_by_last_cmd $primary "sync"]
            assert {$replica_main_conn_id != ""}
            $primary client kill id $replica_main_conn_id
            # Wait for primary to abort the sync
            wait_for_condition 1000 10 {
                [s -1 rdb_bgsave_in_progress] eq 0 &&
                [s -1 rdb_last_bgsave_status] eq "err" 
            } else {
                fail "Primary should abort sync"
            }
        }
        stop_write_load $load_handle
    }
}

start_server {tags {"repl dual-connection external:skip"}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set loglines [count_log_lines 0]

    $primary config set repl-diskless-sync yes
    $primary config set dual-conn-sync-enabled yes
    $primary config set loglevel debug
    $primary config set repl-diskless-sync-delay 0; # don't wait for other replicas

    # Generating RDB will cost 5s(10000 * 0.0001s)
    $primary debug populate 10000 primary 1
    $primary config set rdb-key-save-delay 100
    
    start_server {} {
        set replica_1 [srv 0 client]
        set replica_host_1 [srv 0 host]
        set replica_port_1 [srv 0 port]
        set replica_log_1 [srv 0 stdout]
        
        $replica_1 config set dual-conn-sync-enabled yes
        $replica_1 config set loglevel debug
        $replica_1 config set repl-timeout 10
        start_server {} {
            set replica_2 [srv 0 client]
            set replica_host_2 [srv 0 host]
            set replica_port_2 [srv 0 port]
            set replica_log_2 [srv 0 stdout]
            
            set load_handle [start_write_load $primary_host $primary_port 20]

            $replica_2 config set dual-conn-sync-enabled yes
            $replica_2 config set loglevel debug
            $replica_2 config set repl-timeout 10
            test "Test replica unable to join dual connection sync after started" {
                $replica_1 replicaof $primary_host $primary_port
                # Wait for sync session to start
                wait_for_condition 50 100 {
                    [s -2 rdb_bgsave_in_progress] eq 1
                } else {
                    fail "replica didn't start sync session in time1"
                }
                $replica_2 replicaof $primary_host $primary_port
                wait_for_log_messages -2 {"*Current BGSAVE has socket target. Waiting for next BGSAVE for SYNC*"} $loglines 100 1000
                $primary config set rdb-key-save-delay 0
                # Verify second replica needed new session
                wait_for_sync $replica_2
                assert {[s -2 sync_partial_ok] eq 2}
                assert {[s -2 sync_full] eq 2}
            }
            stop_write_load $load_handle
        }
    }
}
