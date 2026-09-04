# Integration tests for replication throttle (throttle_repl.c).
#
# We drive the throttling by SIGSTOP-ing the replica,
# so its output buffer on the primary grows and never drains.

proc throttle_rate {r} {
    getInfoProperty [{*}$r info throttling] repl_throttle_rate
}

# Check whether the given client is currently throttled.
proc client_throttled {r wid} {
    set flags ""
    regexp {flags=(\S+)} [{*}$r CLIENT LIST ID $wid] -> flags
    string match {*h*} $flags
}

# Keep issuing writes until the writer client is observed being throttled.
proc wait_throttled_client {r writer wid} {
    for {set k 0} {$k < 1000} {incr k} {
        for {set j 0} {$j < 500} {incr j} {
            $writer set nudge v
        }
        if {[client_throttled $r $wid] &&
            [getInfoProperty [{*}$r info debug] repl_throttle_current_clients] > 0} {
            return 1
        }
    }
    return 0
}

# Set up primary/replica replication with throttling enabled and a COB limit configured.
proc setup_throttle_replication {primary replica primary_host primary_port} {
    $primary replicaof no one
    $primary flushall
    $primary config set repl-throttling-enabled yes
    $primary config set repl-backlog-size 1mb
    $primary config set client-output-buffer-limit "replica 1024mb 1mb 3600"
    $primary config set repl-timeout 1800
    $replica replicaof no one
    $replica flushall
    $replica replicaof $primary_host $primary_port
    wait_for_sync $replica
    wait_replica_online $primary
    wait_for_condition 50 100 {
        [throttle_rate $primary] == -1
    } else {
        fail "repl throttler doesn't setup correctly"
    }
}

# Tear down after a test so the next test starts from a clean state.
proc teardown_throttle_replication {primary replica} {

    if {[catch {$primary ping} err]} {
        fail "primary stopped responding during teardown: $err"
    }

    catch {$primary config set repl-throttling-enabled no}
    wait_for_condition 100 100 {
        [throttle_rate $primary] == -1 &&
        [getInfoProperty [$primary info debug] repl_throttle_current_clients] == 0
    } else {
        fail "repl throttler didn't tear down after the test"
    }

    # The replica must be fully synced and hold the same dataset.
    wait_for_sync $replica
    wait_replica_online $primary
    wait_for_ofs_sync $primary $replica
    assert_equal [$primary dbsize] [$replica dbsize]

    # Detach replication
    catch {$replica replicaof no one}
}

start_server {tags {"throttle repl external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_pid [srv 0 pid]
    start_server {} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        test {Steady-state throttle happy case} {
            setup_throttle_replication $primary $replica $primary_host $primary_port

            # Freeze the replica so its output buffer on the primary grows monotonically.
            pause_process $replica_pid

            set writer [valkey_deferring_client]
            $writer CLIENT ID
            set wid [$writer read]

            # Flood writes to grow the replica's COB and activate the throttler.
            for {set i 0} {$i < 5000} {incr i} {
                $writer set key:$i [string repeat x 1000]
            }
            wait_for_condition 50 100 {
                [throttle_rate $primary] >= 0
            } else {
                resume_process $replica_pid
                fail "throttle did not activate while the replica's COB was growing"
            }
            # Keep writing until the client is actually throttled.
            if {![wait_throttled_client $primary $writer $wid]} {
                resume_process $replica_pid
                fail "client was not throttled while the replica's COB was growing"
            }

            set ti [$primary info throttling]
            set td [$primary info debug]
            # Throttling section
            assert {[getInfoProperty $ti repl_throttle_rate] >= 0}
            assert {[getInfoProperty $ti repl_throttle_activation_events] >= 1}
            assert {[getInfoProperty $ti repl_throttle_below_guardrail_secs] >= 0}
            assert {[getInfoProperty $ti repl_throttle_total_commands] > 0}
            assert {[getInfoProperty $ti total_throttled_commands] > 0}

            # Debug section
            assert {[getInfoProperty $td repl_throttle_more_events] >= 1}
            assert {[getInfoProperty $td repl_throttle_less_events] >= 0}
            $writer close
            resume_process $replica_pid

            teardown_throttle_replication $primary $replica
        }

        test {Throttling protects a replica above the soft COB limit} {
            setup_throttle_replication $primary $replica $primary_host $primary_port
            set soft_limit [expr {1 * 1024 * 1024}]
            set hard_limit [expr {1024 * 1024 * 1024}]
            $primary config set client-output-buffer-limit "replica ${hard_limit} ${soft_limit} 0"

            set writer [valkey_deferring_client]
            $writer CLIENT ID
            set wid [$writer read]

            pause_process $replica_pid

            set activated 0
            set payload [string repeat w 2000]
            for {set i 0} {$i < 200 && !$activated} {incr i} {
                for {set j 0} {$j < 200} {incr j} {
                    $writer set key:$j $payload
                }
                if {[throttle_rate $primary] >= 0} {
                    set activated 1
                }
            }
            if {!$activated} {
                resume_process $replica_pid
                fail "throttler never began queueing clients"
            }

            # Write 30MB total (30 x 1MB values). This is well above the 1mb
            # soft limit and well below the 1024mb hard limit, so the replica's
            # COB lands in between.
            set value_size [expr {1 * 1024 * 1024}]
            set num_writes 30
            for {set i 0} {$i < $num_writes} {incr i} {
                $writer set key:$i [string repeat x $value_size]
            }

            if {[status $primary connected_slaves] != 1} {
                resume_process $replica_pid
                fail "replica was disconnected while above soft but below hard COB limit"
            }
            wait_for_condition 50 100 {
                [throttle_rate $primary] >= 0
            } else {
                resume_process $replica_pid
                fail "throttle did not activate while the replica's COB was growing"
            }
            if {![wait_throttled_client $primary $writer $wid]} {
                resume_process $replica_pid
                fail "client was not throttled while the replica's COB was growing"
            }

            $writer close
            resume_process $replica_pid
            teardown_throttle_replication $primary $replica
        }

        test {Throttling not protect a replica above the hard COB limit} {
            setup_throttle_replication $primary $replica $primary_host $primary_port
            $primary config set client-output-buffer-limit "replica 10mb 1mb 0"

            set writer [valkey_deferring_client]
            $writer CLIENT ID
            set wid [$writer read]

            set activation_events_before [getInfoProperty [$primary info throttling] repl_throttle_activation_events]

            pause_process $replica_pid

            set activated 0
            set payload [string repeat w 2000]
            for {set i 0} {$i < 200 && !$activated} {incr i} {
                for {set j 0} {$j < 200} {incr j} {
                    $writer set key:$j $payload
                }
                if {[throttle_rate $primary] >= 0} {
                    set activated 1
                }
            }
            if {!$activated} {
                resume_process $replica_pid
                fail "throttler never began queueing clients"
            }

            # Write 100MB total (100 x 1MB values). This is well above the 10mb
            # hard limit, so the replica will be disconnected.
            set value_size [expr {1 * 1024 * 1024}]
            set num_writes 100
            for {set i 0} {$i < $num_writes} {incr i} {
                $writer set key:$i [string repeat x $value_size]
            }

            wait_for_condition 50 100 {
                [throttle_rate $primary] == -1 &&
                ![client_throttled $primary $wid] &&
                [status $primary connected_slaves] == 0
            } else {
                resume_process $replica_pid
                fail "throttle did not tear down after the replica was disconnected"
            }

            set activation_events_after [getInfoProperty [$primary info throttling] repl_throttle_activation_events]
            assert {$activation_events_after > $activation_events_before}

            $writer close
            resume_process $replica_pid
            teardown_throttle_replication $primary $replica
        }

        test {Throttling tears down when failover happened} {
            setup_throttle_replication $primary $replica $primary_host $primary_port

            pause_process $replica_pid
            set writer [valkey_deferring_client]
            $writer CLIENT ID
            set wid [$writer read]

            # Activate throttling.
            for {set i 0} {$i < 5000} {incr i} {
                $writer set fkey:$i [string repeat z 1000]
            }
            wait_for_condition 50 100 {
                [throttle_rate $primary] >= 0
            } else {
                resume_process $replica_pid
                fail "throttle did not activate before failover"
            }

            if {![wait_throttled_client $primary $writer $wid]} {
                resume_process $replica_pid
                fail "Client is not throttled."
            }

            # Trigger the failover. The throttling must tear
            # down even though the still-frozen replica's COB is high.
            $primary replicaof $replica_host $replica_port
            wait_for_condition 50 100 {
                [throttle_rate $primary] == -1 && ![client_throttled $primary $wid]
            } else {
                resume_process $replica_pid
                fail "throttle was not torn down after the primary was demoted"
            }

            $writer close
            resume_process $replica_pid
            teardown_throttle_replication $replica $primary
        }

        test {Throttling tears down when disabling config} {
            setup_throttle_replication $primary $replica $primary_host $primary_port

            pause_process $replica_pid
            set writer [valkey_deferring_client]
            $writer CLIENT ID
            set wid [$writer read]

            # Activate throttling.
            for {set i 0} {$i < 5000} {incr i} {
                $writer set key:$i [string repeat w 1000]
            }
            wait_for_condition 50 100 {
                [throttle_rate $primary] >= 0
            } else {
                resume_process $replica_pid
                fail "Throttler did not activate."
            }
            if {![wait_throttled_client $primary $writer $wid]} {
                resume_process $replica_pid
                fail "Client is not throttled."
            }

            # Disable the feature while COB is still high. The throttler must tear
            # down AND release its throttled client.
            $primary config set repl-throttling-enabled no
            wait_for_condition 50 100 {
                [throttle_rate $primary] == -1 && ![client_throttled $primary $wid]
            } else {
                resume_process $replica_pid
                fail "Throttle not torn down / client not released after disabling steady state throttling."
            }

            $writer close
            resume_process $replica_pid
            teardown_throttle_replication $primary $replica
        }

        test {Client disconnect while throttling} {
            setup_throttle_replication $primary $replica $primary_host $primary_port

            pause_process $replica_pid
            set writer [valkey_deferring_client]
            $writer CLIENT ID
            set wid [$writer read]

            # Activate throttling.
            for {set i 0} {$i < 5000} {incr i} {
                $writer set key:$i [string repeat w 1000]
            }
            wait_for_condition 50 100 {
                [throttle_rate $primary] >= 0
            } else {
                resume_process $replica_pid
                fail "Throttler did not activate."
            }
            if {![wait_throttled_client $primary $writer $wid]} {
                resume_process $replica_pid
                fail "Client is not throttled."
            }

            # While throttled, queue 500 counter increments. They are buffered behind
            # the throttle, not executed yet.
            for {set i 0} {$i < 500} {incr i} {
                $writer incr counter
            }

            # The client drops its own TCP connection while throttled.
            # It must then be removed from the throttler.
            $writer close
            wait_for_condition 50 100 {
                [getInfoProperty [$primary info debug] repl_throttle_current_clients] == 0 &&
                ![client_throttled $primary $wid]
            } else {
                resume_process $replica_pid
                fail "throttled client was not removed after its connection dropped"
            }

            # The disconnect prevented the buffered increments from all running.
            set executed [$primary get counter]
            if {$executed eq ""} {set executed 0}
            assert {$executed < 500}

            resume_process $replica_pid
            teardown_throttle_replication $primary $replica
        }

        test {Client blocked before throttling and unblocked after throttling} {
            setup_throttle_replication $primary $replica $primary_host $primary_port

            # Block on a key BEFORE any repl throttler exists.
            set blocker [valkey_deferring_client]
            $blocker blpop mylist 0
            wait_for_blocked_client
            pause_process $replica_pid

            # Drive the replica COB up with a deferring writer until the throttler
            # queues this client.
            set writer [valkey_deferring_client]
            $writer CLIENT ID
            set wid [$writer read]
            set throttled 0
            set payload [string repeat w 2000]
            for {set i 0} {$i < 200 && !$throttled} {incr i} {
                for {set j 0} {$j < 200} {incr j} {
                    $writer set key:$j $payload
                }
                if {[client_throttled $primary $wid]} {
                    set throttled 1
                }
            }
            if {!$throttled} {
                resume_process $replica_pid
                fail "throttler never began queueing clients"
            }

            # Deferring hosers that never read their replies, so the token bucket
            # is empty and the throttler queue is non-empty when the LPUSH lands.
            set writers {}
            for {set i 0} {$i < 4} {incr i} {
                lappend writers [valkey_deferring_client]
            }

            # Nothing may be read from the primary between this burst and the
            # LPUSH. Commands are processed in arrival order, so the LPUSH lands
            # behind the burst.
            foreach w $writers {
                for {set j 0} {$j < 500} {incr j} {
                    $w set key:$j $payload
                }
            }
            set pusher [valkey_deferring_client]
            $pusher lpush mylist v

            resume_process $replica_pid
            wait_for_sync $replica
            wait_replica_online $primary

            catch {$blocker close}
            catch {$pusher close}
            catch {$writer close}
            foreach w $writers { catch {$w close} }
            teardown_throttle_replication $primary $replica
        }
    }
}
