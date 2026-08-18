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
    for {set k 0} {$k < 200} {incr k} {
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

start_server {tags {"throttle repl external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_pid [srv 0 pid]
    start_server {} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        proc setup_replication {} {
            uplevel 1 {
                $primary replicaof no one
                $primary flushall
                $primary config set repl-throttling-enabled yes
                $primary config set client-output-buffer-limit "replica 1024mb 64kb 3600"
                $replica replicaof no one
                $replica flushall
                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica
                wait_replica_online $primary
            }
        }

        test {Steady-state throttle happy case} {
            setup_replication

            # Freeze the replica so its output buffer on the primary grows monotonically.
            exec kill -SIGSTOP $replica_pid

            # Flood writes, activate throttling.
            set writer [valkey_deferring_client]
            for {set i 0} {$i < 5000} {incr i} {
                $writer set key:$i [string repeat x 1000]
            }

            # Throttler is activated and reports a non-negative rate.
            wait_for_condition 50 100 {
                [throttle_rate $primary] >= 0
            } else {
                exec kill -SIGCONT $replica_pid
                fail "throttle did not activate while the replica's COB was growing"
            }

            $writer close
            exec kill -SIGCONT $replica_pid

            assert {[getInfoProperty [$primary info throttling] repl_throttle_activation_events] >= 1}

            # As the replica catches up, the throttler ramps its rate back to
            # unlimited and uninstalls, reporting an inactive rate (-1) again. The
            # ramp-up is gradual climbing back, so allow a generous window.
            wait_for_condition 300 100 {
                [throttle_rate $primary] == -1
            } else {
                fail "throttle did not release after the replica caught up"
            }

            # The replica must be fully synced and hold the same dataset.
            wait_for_ofs_sync $primary $replica
            assert_equal [$primary dbsize] [$replica dbsize]
        }

        test {Hard COB limit disconnects a lagging replica while throttling} {
            setup_replication
            $primary config set client-output-buffer-limit "replica 4mb 64kb 3600"

            exec kill -SIGSTOP $replica_pid
            set writer [valkey_deferring_client]

            # Use large values to make the COB past the hard limit.
            for {set i 0} {$i < 500} {incr i} {
                $writer set hkey:$i [string repeat y 100000]
            }

            # Once COB exceeds the hard limit, the replica is disconnected.
            wait_for_condition 100 100 {
                [status $primary connected_slaves] == 0
            } else {
                exec kill -SIGCONT $replica_pid
                fail "replica was not disconnected after exceeding the hard COB limit"
            }

            $writer close
            exec kill -SIGCONT $replica_pid
        }

        test {Throttling tears down when failover happened} {
            setup_replication

            exec kill -SIGSTOP $replica_pid
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
                exec kill -SIGCONT $replica_pid
                fail "throttle did not activate before failover"
            }

            if {![wait_throttled_client $primary $writer $wid]} {
                exec kill -SIGCONT $replica_pid
                fail "Client is not throttled."
            }

            # Trigger the failover. The throttling must tear
            # down even though the still-frozen replica's COB is high.
            $primary replicaof $replica_host $replica_port
            wait_for_condition 50 100 {
                [throttle_rate $primary] == -1 && ![client_throttled $primary $wid]
            } else {
                exec kill -SIGCONT $replica_pid
                fail "throttle was not torn down after the primary was demoted"
            }

            $writer close
            exec kill -SIGCONT $replica_pid
        }

        test {Throttling tears down when disabling config} {
            setup_replication

            exec kill -SIGSTOP $replica_pid
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
                exec kill -SIGCONT $replica_pid
                fail "Throttler did not activate."
            }
            if {![wait_throttled_client $primary $writer $wid]} {
                exec kill -SIGCONT $replica_pid
                fail "Client is not throttled."
            }

            # Disable the feature while COB is still high. The throttler must tear
            # down AND release its throttled client.
            $primary config set repl-throttling-enabled no
            wait_for_condition 50 100 {
                [throttle_rate $primary] == -1 && ![client_throttled $primary $wid]
            } else {
                exec kill -SIGCONT $replica_pid
                fail "Throttle not torn down / client not released after disabling steady state throttling."
            }

            $writer close
            exec kill -SIGCONT $replica_pid
        }

        test {Client disconnect while throttling} {
            setup_replication

            exec kill -SIGSTOP $replica_pid
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
                exec kill -SIGCONT $replica_pid
                fail "Throttler did not activate."
            }
            if {![wait_throttled_client $primary $writer $wid]} {
                exec kill -SIGCONT $replica_pid
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
                exec kill -SIGCONT $replica_pid
                fail "throttled client was not removed after its connection dropped"
            }

            # The disconnect prevented the buffered increments from all running.
            set executed [$primary get counter]
            if {$executed eq ""} {set executed 0}
            assert {$executed < 500}

            exec kill -SIGCONT $replica_pid
        }
    }
}
