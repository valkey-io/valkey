proc log_file_matches {log pattern} {
    set fp [open $log r]
    set content [read $fp]
    close $fp
    string match $pattern $content
}

start_server {tags {"repl rdb-connection external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_log [srv 0 stdout]
    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        # Configure the master in order to hang waiting for the BGSAVE
        # operation, so that the replica remains in the handshake state.
        $master config set repl-diskless-sync yes
        $master config set repl-diskless-sync-delay 1000
        $master config set repl-rdb-connection yes

        # Start the replication process...
        $replica config set repl-rdb-connection yes
        $replica slaveof $master_host $master_port

        test {Test repl-rdb-connection replica enters handshake} {
            wait_for_condition 50 1000 {
                [string match *handshake* [$replica role]]
            } else {
                fail "Replica does not enter handshake state"
            }
        }

        test {Test repl-rdb-connection enters wait_bgsave} {
            wait_for_condition 50 1000 {
                [string match *state=wait_bgsave* [$master info replication]]
            } else {
                fail "Replica does not enter wait_bgsave state"
            }
        }

        $master config set repl-diskless-sync-delay 0

        test {Test repl-rdb-connection replica is able to sync} {
            verify_replica_online $master 0 500
            wait_for_condition 50 1000 {
                [string match *connected_slaves:1* [$master info]]
            } else {
                fail "Replica rdb connection is still open"
            }
            set offset [status $master master_repl_offset]
            wait_for_condition 500 100 {
                [string match "*slave0:*,offset=$offset,*" [$master info replication]] &&
                $offset == [status $replica master_repl_offset]
            } else {
                fail "Replicas and master offsets were unable to match."
            }
        }
    }
}

start_server {tags {"repl rdb-connection external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_log [srv 0 stdout]
    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        $master config set rdb-key-save-delay 200
        $master config set repl-rdb-connection yes
        $replica config set repl-rdb-connection yes
        $replica config set repl-diskless-sync no

        populate 1000 master 10000
        set load_handle1 [start_one_key_write_load $master_host $master_port 100 "mykey1"]
        set load_handle2 [start_one_key_write_load $master_host $master_port 100 "mykey2"]
        set load_handle3 [start_one_key_write_load $master_host $master_port 100 "mykey3"]
        
        # wait for load handlers to start
        wait_for_condition 50 1000 {
            ([$master get "mykey1"] != "") &&
            ([$master get "mykey2"] != "") &&
            ([$master get "mykey3"] != "")
        } else {
            fail "Can't set new keys"
        }

        set before_used [s 0 used_memory]

        test {Primary memory usage does not increase during rdb-connection sync} {        
            $replica slaveof $master_host $master_port

            # Verify used_memory stays low through all the sync
            set max_retry 500
            while {$max_retry} {
                # Verify memory
                set used_memory [s 0 used_memory]
                assert {$used_memory-$before_used <= 1.5*10^6}; # ~1/3 of the space
                # Check replica state
                set master_info [$master info]
                set replica_info [$replica info]
                if {[string match *slave0:*state=online* $master_info] &&
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

        test {Steady state after rdb channel sync} {
            set val1 [$master get mykey1]
            set val2 [$master get mykey2]
            set val3 [$master get mykey3]
            wait_for_condition 50 1000 {
                ([$replica get "mykey1"] eq $val1) &&
                ([$replica get "mykey2"] eq $val2) &&
                ([$replica get "mykey3"] eq $val3)
            } else {
                fail "Can't set new keys"
            }
        }

        test {Rollback to nornal sync} {
            $replica slaveof no one
            $replica config set repl-rdb-connection no
            $master set newkey newval

            set sync_full [s 0 sync_full]
            assert {$sync_full > 0}

            $replica slaveof $master_host $master_port
            verify_replica_online $master 0 500
            assert_equal [expr $sync_full + 1] [s 0 sync_full]
            assert [string match *connected_slaves:1* [$master info]]
        }
    }
}

start_server {tags {"repl rdb-connection external:skip"}} {
    foreach start_with_rdb_sync_enabled {yes no} {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]
        set replica_log [srv 0 stdout]
        start_server {} {
            set master [srv 0 client]
            set master_host [srv 0 host]
            set master_port [srv 0 port]

            $master config set repl-diskless-sync yes
            $master config set client-output-buffer-limit "replica 1100k 0 0"
            $replica config set repl-rdb-connection $start_with_rdb_sync_enabled

            test "Test enable disable repl-rdb-connection start with $start_with_rdb_sync_enabled" {
                # Set master shared replication buffer size to a bit more then the size of 
                # a replication buffer block.
                
                populate 1000 master 10000

                $replica slaveof $master_host $master_port
                verify_replica_online $master 0 500

                set sync_full [s 0 sync_full]
                assert {$sync_full > 0}

                $replica slaveof no one
                if {$start_with_rdb_sync_enabled == "yes"} {
                    # disable rdb channel sync
                    $replica config set repl-rdb-connection no
                } else {
                    $replica config set repl-rdb-connection yes
                }

                # Force replica to full sync next time
                populate 1000 master 10000

                $replica slaveof $master_host $master_port
                verify_replica_online $master 0 500
                wait_for_sync $replica

                wait_for_condition 100 100 {
                    [s 0 sync_full] > $sync_full
                } else {
                    fail "Master <-> Replica didn't start the full sync"
                }
            }
        }
    }
}

start_server {tags {"repl rdb-connection external:skip"}} {
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
            set master [srv 0 client]
            set master_host [srv 0 host]
            set master_port [srv 0 port]
            set loglines [count_log_lines -1]

            populate 10000 master 10000
            $master set key1 val1

            $master config set repl-diskless-sync yes
            $master config set repl-diskless-sync-delay 5; # allow both replicas to ask for sync
            $master config set repl-rdb-connection yes

            $replica1 config set repl-rdb-connection yes
            $replica2 config set repl-rdb-connection yes
            $replica1 config set repl-diskless-sync no
            $replica2 config set repl-diskless-sync no
            $replica1 config set loglevel debug
            $replica2 config set loglevel debug

            test "Two replicas in one sync session with repl-rdb-connection" {
                $replica1 slaveof $master_host $master_port
                $replica2 slaveof $master_host $master_port

                wait_for_value_to_propegate_to_replica $master $replica1 "key1"
                wait_for_value_to_propegate_to_replica $master $replica2 "key1"

                wait_for_condition 100 100 {
                    [s 0 total_forks] eq "1" 
                } else {
                    fail "Master <-> Replica didn't start the full sync"
                }

                verify_replica_online $master 0 500
                verify_replica_online $master 1 500
                wait_for_condition 50 1000 {
                    [status $replica1 master_link_status] == "up"
                } else {
                    fail "Replica is not synced"
                }           
            }

            $replica1 slaveof no one
            $replica2 slaveof no one

            $replica1 config set repl-rdb-connection yes
            $replica2 config set repl-rdb-connection no

            $master set key2 val2

            test "Test one replica with repl-rdb-connection enabled one with disabled" {
                $replica1 slaveof $master_host $master_port
                $replica2 slaveof $master_host $master_port

                wait_for_value_to_propegate_to_replica $master $replica1 "key2"
                wait_for_value_to_propegate_to_replica $master $replica2 "key2"

                verify_replica_online $master 0 500
                verify_replica_online $master 1 500
                wait_for_condition 50 1000 {
                    [status $replica1 master_link_status] == "up"
                } else {
                    fail "Replica is not synced"
                }
            }

            $replica1 slaveof no one
            $master set key4 val4            
            
            test "Test replica's buffer limit reached" {
                $master config set repl-diskless-sync-delay 0
                $master config set rdb-key-save-delay 500
                # At this point we have about 10k keys in the db, 
                # We expect that the next full sync will take 5 seconds (10k*500)ms
                # It will give us enough time to fill the replica buffer.
                $replica1 config set repl-rdb-connection yes
                $replica1 config set client-output-buffer-limit "replica 16383 16383 0"
                $replica1 config set loglevel debug

                $replica1 slaveof $master_host $master_port
                # Wait for replica to establish psync using main connection
                wait_for_condition 50 1000 {
                    [log_file_matches $replica1_log "*Primary accepted a Partial Resynchronization, RDB load in background.*"]
                } else {
                    fail "Psync hasn't been established"
                }

                populate 10000 master 10000
                # Wait for replica's buffer limit reached
                wait_for_condition 50 1000 {
                    [log_file_matches $replica1_log "*Replication buffer limit reached, stopping buffering*"]
                } else {
                    fail "Replica buffer should fill"
                }
                assert {[s -2 replicas_replication_buffer_size] <= 16385*2}

                # Wait for sync to succeed 
                wait_for_value_to_propegate_to_replica $master $replica1 "key4"
                wait_for_condition 50 1000 {
                    [status $replica1 master_link_status] == "up"
                } else {
                    fail "Replica is not synced"
                }
            }

            $replica1 slaveof no one
            $replica1 config set client-output-buffer-limit "replica 256mb 256mb 0"; # remove repl buffer limitation

            $master set key5 val5
            
            test "Rdb-channel-sync fails when primary diskless disabled" {
                set cur_psync [status $master sync_partial_ok]
                $master config set repl-diskless-sync no

                $replica1 config set repl-rdb-connection yes
                $replica1 slaveof $master_host $master_port

                # Wait for mitigation and resync
                wait_for_value_to_propegate_to_replica $master $replica1 "key5"

                wait_for_condition 50 1000 {
                    [status $replica1 master_link_status] == "up"
                } else {
                    fail "Replica is not synced"
                }

                # Verify that we did not use rdb-connection sync
                assert {[status $master sync_partial_ok] == $cur_psync}
            }
        }
    }
}

start_server {tags {"repl rdb-connection external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_log [srv 0 stdout]
    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]
        set loglines [count_log_lines -1]
        # Create small enough db to be loaded before replica establish psync connection
        $master set key1 val1

        $master config set repl-diskless-sync yes
        $master debug sleep-after-fork [expr {5 * [expr {10 ** 6}]}];# Stop master after fork for 5 seconds
        $master config set repl-rdb-connection yes

        $replica config set repl-rdb-connection yes
        $replica config set loglevel debug

        test "Test rdb-connection psync established after rdb load" {
            $replica slaveof $master_host $master_port

            verify_replica_online $master 0 500
            wait_for_condition 50 1000 {
                [status $replica master_link_status] == "up"
            } else {
                fail "Replica is not synced"
            }
            wait_for_value_to_propegate_to_replica $master $replica "key1"
            # Confirm the occurrence of a race condition.
            set res [wait_for_log_messages -1 {"*RDB connection sync - psync established after rdb load*"} $loglines 2000 1]
            set loglines [lindex $res 1]
            incr $loglines                
        }
    }
}

start_server {tags {"repl rdb-connection external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_log [srv 0 stdout]
    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]
        set backlog_size [expr {10 ** 5}]
        set loglines [count_log_lines -1]

        $master config set repl-diskless-sync yes
        $master config set repl-rdb-connection yes
        $master config set repl-backlog-size $backlog_size
        $master config set loglevel debug
        $master config set repl-timeout 10
        $master config set rdb-key-save-delay 200
        populate 10000 master 10000
        
        set load_handle1 [start_one_key_write_load $master_host $master_port 100 "mykey1"]
        set load_handle2 [start_one_key_write_load $master_host $master_port 100 "mykey2"]
        set load_handle3 [start_one_key_write_load $master_host $master_port 100 "mykey3"]

        $replica config set repl-rdb-connection yes
        $replica config set loglevel debug
        $replica config set repl-timeout 10
        # Stop replica after master fork for 5 seconds
        $replica debug sleep-after-fork [expr {5 * [expr {10 ** 6}]}]

        test "Test rdb-connection connection peering - replica able to establish psync" {
            $replica slaveof $master_host $master_port
            # Verify repl backlog can grow
            wait_for_condition 1000 10 {
                [s 0 mem_total_replication_buffers] > [expr {2 * $backlog_size}]
            } else {
                fail "Master should allow backlog to grow beyond its limits during rdb-connection sync handshake"
            }

            verify_replica_online $master 0 500
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

start_server {tags {"repl rdb-connection external:skip"}} {
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
            set master [srv 0 client]
            set master_host [srv 0 host]
            set master_port [srv 0 port]
            set backlog_size [expr {10 ** 6}]
            set loglines [count_log_lines -1]

            $master config set repl-diskless-sync yes
            $master config set repl-rdb-connection yes
            $master config set repl-backlog-size $backlog_size
            $master config set loglevel debug
            $master config set repl-timeout 10
            $master config set rdb-key-save-delay 10
            populate 1024 master 16
            
            set load_handle0 [start_write_load $master_host $master_port 20]

            $replica1 config set repl-rdb-connection yes
            $replica2 config set repl-rdb-connection yes
            $replica1 config set loglevel debug
            $replica2 config set loglevel debug
            $replica1 config set repl-timeout 10
            $replica2 config set repl-timeout 10

            # Stop replica after master fork for 2 seconds
            $replica1 debug sleep-after-fork [expr {2 * [expr {10 ** 6}]}]
            $replica2 debug sleep-after-fork [expr {2 * [expr {10 ** 6}]}]
            test "Test rdb-connection connection peering - start with empty backlog (retrospect)" {
                $replica1 slaveof $master_host $master_port
                set res [wait_for_log_messages 0 {"*Add replica * repl-backlog is empty*"} $loglines 2000 1]
                set res [wait_for_log_messages 0 {"*Retrospect attach replica*"} $loglines 2000 1]
                set loglines [lindex $res 1]
                incr $loglines 
                verify_replica_online $master 0 700
                wait_for_condition 50 1000 {
                    [status $replica1 master_link_status] == "up"
                } else {
                    fail "Replica is not synced"
                }
                $replica1 slaveof no one
                assert [string match *replicas_waiting_psync:0* [$master info replication]]
            }

            test "Test rdb-connection connection peering - start with backlog" {
                $replica2 slaveof $master_host $master_port
                set res [wait_for_log_messages 0 {"*Add replica * with repl-backlog tail*"} $loglines 2000 1]
                set loglines [lindex $res 1]
                incr $loglines 
                verify_replica_online $master 0 700
                wait_for_condition 50 1000 {
                    [status $replica2 master_link_status] == "up"
                } else {
                    fail "Replica is not synced"
                }
                assert [string match *replicas_waiting_psync:0* [$master info replication]]
            }

            stop_write_load $load_handle0
        }
    }
}

proc start_bg_server_sleep {host port sec} {
    set tclsh [info nameofexecutable]
    exec $tclsh tests/helpers/bg_server_sleep.tcl $host $port $sec &
}

proc stop_bg_server_sleep {handle} {
    catch {exec /bin/kill -9 $handle}
}

start_server {tags {"repl rdb-connection external:skip"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    
    $master config set repl-diskless-sync yes
    $master config set repl-rdb-connection yes
    $master config set repl-backlog-size [expr {10 ** 6}]
    $master config set loglevel debug
    $master config set repl-timeout 10
    # generate small db
    populate 10 master 10
    # Stop master main process after fork for 2 seconds
    $master debug sleep-after-fork [expr {2 * [expr {10 ** 6}]}]
    $master debug wait-before-rdb-client-free 5

    start_server {} {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]
        set replica_log [srv 0 stdout]
        set loglines [count_log_lines 0]
        
        set load_handle0 [start_write_load $master_host $master_port 20]

        $replica config set repl-rdb-connection yes
        $replica config set loglevel debug
        $replica config set repl-timeout 10

        test "Test rdb-connection psync established after rdb load, master keep repl-block" {
            $replica slaveof $master_host $master_port
            set res [wait_for_log_messages 0 {"*Done loading RDB*"} $loglines 2000 1]
            set loglines [lindex $res 1]
            incr $loglines
            # At this point rdb is loaded but psync hasn't been established yet. 
            # Force the replica to sleep for 3 seconds so the master main process will wake up, while the replica is unresponsive.
            set sleep_handle [start_bg_server_sleep $replica_host $replica_port 3]
            wait_for_condition 50 100 {
                [string match {*replicas_waiting_psync:1*} [$master info replication]]
            } else {
                fail "Master freed RDB client before psync was established"
            }

            verify_replica_online $master 0 500
            wait_for_condition 50 100 {
                [string match {*replicas_waiting_psync:0*} [$master info replication]]
            } else {
                fail "Master did not free repl buf block after psync establishment"
            }
            $replica slaveof no one
        }
        stop_write_load $load_handle0
    }

    start_server {} {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]
        set replica_log [srv 0 stdout]
        set loglines [count_log_lines 0]
        
        set load_handle0 [start_write_load $master_host $master_port 20]

        $replica config set repl-rdb-connection yes
        $replica config set loglevel debug
        $replica config set repl-timeout 10

        test "Test rdb-connection psync established after rdb load, master dismiss repl-block" {
            $replica slaveof $master_host $master_port
            set res [wait_for_log_messages 0 {"*Done loading RDB*"} $loglines 2000 1]
            set loglines [lindex $res 1]
            incr $loglines
            # At this point rdb is loaded but psync hasn't been established yet. 
            # Force the replica to sleep for 8 seconds so the master main process will wake up, while the replica is unresponsive.
            # We expect the grace time to be over before the replica wake up, so sync will fail.
            set sleep_handle [start_bg_server_sleep $replica_host $replica_port 8]
            wait_for_condition 50 100 {
                [string match {*replicas_waiting_psync:1*} [$master info replication]]
            } else {
                fail "Master should wait before freeing repl block"
            }

            # Sync should fail once the replica ask for PSYNC using main channel
            set res [wait_for_log_messages -1 {"*Replica main connection failed to establish PSYNC within the grace period*"} 0 4000 1]

            # Should succeed on retry
            verify_replica_online $master 0 500
            wait_for_condition 50 100 {
                [string match {*replicas_waiting_psync:0*} [$master info replication]]
            } else {
                fail "Master did not free repl buf block after psync establishment"
            }
            $replica slaveof no one
        }
        stop_write_load $load_handle0
    }
}

start_server {tags {"repl rdb-connection external:skip"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    set loglines [count_log_lines 0]

    $master config set repl-diskless-sync yes
    $master config set repl-rdb-connection yes
    $master config set client-output-buffer-limit "replica 1100k 0 0"
    $master config set loglevel debug
    # generate small db
    populate 10 master 10
    # Stop master main process after fork for 1 seconds
    $master debug sleep-after-fork [expr {2 * [expr {10 ** 6}]}]
    start_server {} {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]
        set replica_log [srv 0 stdout]
        
        set load_handle0 [start_write_load $master_host $master_port 20]
        set load_handle1 [start_write_load $master_host $master_port 20]
        set load_handle2 [start_write_load $master_host $master_port 20]

        $replica config set repl-rdb-connection yes
        $replica config set loglevel debug
        $replica config set repl-timeout 10

        test "Test rdb-connection master gets cob overrun before established psync" {
            $replica slaveof $master_host $master_port
            wait_for_log_messages 0 {"*Done loading RDB*"} 0 2000 1

            # At this point rdb is loaded but psync hasn't been established yet. 
            # Force the replica to sleep for 5 seconds so the master main process will wake up while the
            # replica is unresponsive. We expect the main process to fill the COB before the replica wakes.
            set sleep_handle [start_bg_server_sleep $replica_host $replica_port 5]
            wait_for_log_messages -1 {"*Client * closed * for overcoming of output buffer limits.*"} $loglines 2000 1
            wait_for_condition 50 100 {
                [string match {*replicas_waiting_psync:0*} [$master info replication]]
            } else {
                fail "Master did not free repl buf block after sync failure"
            }
            set res [wait_for_log_messages -1 {"*Unable to partial resync with replica * for lack of backlog*"} $loglines 20000 1]
            set loglines [lindex $res 1]
        }

        $replica slaveof no one
        $replica debug sleep-after-fork [expr {2 * [expr {10 ** 6}]}]

        $master debug populate 1000 master 100000
        # Set master with a slow rdb generation, so that we can easily intercept loading
        # 10ms per key, with 1000 keys is 10 seconds
        $master config set rdb-key-save-delay 10000
        $master debug sleep-after-fork 0

        test "Test rdb-connection master gets cob overrun during replica rdb load" {
            $replica slaveof $master_host $master_port

            wait_for_log_messages -1 {"*Client * closed * for overcoming of output buffer limits.*"} $loglines 2000 1
            wait_for_condition 50 100 {
                [string match {*replicas_waiting_psync:0*} [$master info replication]]
            } else {
                fail "Master did not free repl buf block after sync failure"
            }
            set res [wait_for_log_messages -1 {"*Unable to partial resync with replica * for lack of backlog*"} $loglines 20000 1]
            set loglines [lindex $res 0]
        }
        stop_write_load $load_handle0
        stop_write_load $load_handle1
        stop_write_load $load_handle2
    }
}

foreach rdbchan {yes no} {
start_server {tags {"repl rdb-connection external:skip"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    set loglines [count_log_lines 0]

    $master config set repl-diskless-sync yes
    $master config set repl-rdb-connection yes
    $master config set loglevel debug
    $master config set repl-diskless-sync-delay 5
    
    # Generating RDB will cost 5s(10000 * 0.0005s)
    $master debug populate 10000 master 1
    $master config set rdb-key-save-delay 500

    $master config set repl-rdb-connection $rdbchan

    start_server {} {
        set replica1 [srv 0 client]
        $replica1 config set repl-rdb-connection $rdbchan
        $replica1 config set loglevel debug
        start_server {} {
            set replica2 [srv 0 client]
            $replica2 config set repl-rdb-connection $rdbchan
            $replica2 config set loglevel debug
            $replica2 config set repl-timeout 60

            set load_handle [start_one_key_write_load $master_host $master_port 100 "mykey1"]
            test "Sync should continue if not all slaves dropped rdb-connection $rdbchan" {
                $replica1 slaveof $master_host $master_port
                $replica2 slaveof $master_host $master_port

                wait_for_condition 50 1000 {
                    [status $master rdb_bgsave_in_progress] == 1
                } else {
                    fail "Sync did not start"
                }
                if {$rdbchan == "yes"} {
                    # Wait for both replicas main conns to establish psync
                    wait_for_condition 50 1000 {
                        [status $master sync_partial_ok] == 2
                    } else {
                        fail "Replicas main conns didn't establish psync [status $master sync_partial_ok]"
                    }
                }

                catch {$replica1 shutdown nosave}
                wait_for_condition 50 2000 {
                    [status $replica2 master_link_status] == "up" &&
                    [status $master sync_full] == 2 &&
                    (($rdbchan == "yes" && [status $master sync_partial_ok] == 2) || $rdbchan == "no")
                } else {
                    fail "Sync session interapted\n
                        sync_full:[status $master sync_full]\n
                        sync_partial_ok:[status $master sync_partial_ok]"
                }
            }
            
            $replica2 slaveof no one

            # Generating RDB will cost 500s(1000000 * 0.0001s)
            $master debug populate 1000000 master 1
            $master config set rdb-key-save-delay 100
    
            test "Master abort sync if all slaves dropped rdb-connection $rdbchan" {
                set cur_psync [status $master sync_partial_ok]
                $replica2 slaveof $master_host $master_port

                wait_for_condition 50 1000 {
                    [status $master rdb_bgsave_in_progress] == 1
                } else {
                    fail "Sync did not start"
                }
                if {$rdbchan == "yes"} {
                    # Wait for both replicas main conns to establish psync
                    wait_for_condition 50 1000 {
                        [status $master sync_partial_ok] == $cur_psync + 1
                    } else {
                        fail "Replicas main conns didn't establish psync [status $master sync_partial_ok]"
                    }
                }

                catch {$replica2 shutdown nosave}
                wait_for_condition 50 1000 {
                    [status $master rdb_bgsave_in_progress] == 0
                } else {
                    fail "Master should abort the sync"
                }
            }
            stop_write_load $load_handle
        }
    }
}
}
