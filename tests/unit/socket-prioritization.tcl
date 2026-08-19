# Copyright (c) Valkey Contributors
# All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause

start_server {tags {"socket-prioritization"}} {
    test {Socket Prioritization: CLIENT LIST and CLIENT KILL QOS filter} {
        set c1 [valkey_client]
        $c1 client setname qostestclient

        # By default client is normal priority
        set res [r client list not-flags H name qostestclient]
        assert_match "*name=qostestclient*flags=N*" $res

        set res_high [r client list flags H name qostestclient]
        assert_equal "" $res_high

        # Test invalid flags argument
        assert_error "*Unknown flags*" {r client list flags invalid_flag}

        # Kill client by flags
        set killed [r client kill not-flags H name qostestclient]
        assert_equal 1 $killed
        assert_error "*I/O error*" {$c1 ping}
        catch {$c1 close}
    }

    test {Socket Prioritization: Replication QoS classification and CLIENT KILL} {
        # Start a replica server and verify that replication links are upgraded to QoS priority
        start_server {} {
            set replica [srv 0 client]
            set replica_host [srv 0 host]
            set replica_port [srv 0 port]
            set primary [srv -1 client]
            set primary_host [srv -1 host]
            set primary_port [srv -1 port]

            # Connect replica to primary
            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 100 {
                [string match "*role:slave*master_link_status:up*" [$replica info replication]]
            } else {
                fail "Can't turn the instance into a replica"
            }

            # On primary server, verify that a client connection has flags=H for the replica
            set rep_list [$primary client list flags H]
            assert_match "*flags=*H*" $rep_list

            set val [string repeat "a" 1024]
            for {set i 0} {$i < 50} {incr i} {
                $primary set "key:$i" $val
            }
            $primary ping

            # Wait for replica to sync the keys
            wait_for_condition 50 100 {
                [$replica dbsize] == 50
            } else {
                fail "Replica failed to sync 50 keys"
            }

            # Verify CLIENT KILL flags H kills the replica connection
            set killed [$primary client kill flags H]
            assert {$killed >= 1}
        }
    } {} {external:skip}

    foreach io_threads {1 4} {
        test "Verify replication connection upgrade (io-threads=$io_threads)" {
            set primary [srv 0 client]
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]

            $primary CONFIG SET io-threads $io_threads

            set replica_mock [valkey $primary_host $primary_port 0 $::tls]
            $replica_mock client setname replica_mock_$io_threads
            
            set res [$primary client list not-flags H name replica_mock_$io_threads]
            assert_match "*name=replica_mock_$io_threads*flags=N*" $res
            assert_equal "" [$primary client list flags H name replica_mock_$io_threads]
            
            $replica_mock write "PSYNC ? -1\r\n"
            $replica_mock flush
            
            wait_for_condition 50 100 {
                [string match "*name=replica_mock_${io_threads}*flags=*H*" [$primary client list flags H name replica_mock_$io_threads]]
            } else {
                fail "Replica connection was not upgraded to QoS priority (io-threads=$io_threads)"
            }
            
            $replica_mock close
            $primary CONFIG SET io-threads 1
        }
    }
}

start_server {tags {"socket-prioritization external:skip"}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    test "Populate standalone primary with 1000 keys" {
        for {set k 0} {$k < 1000} {incr k} {
            $primary set "benchkey:$k" [string repeat "x" 128]
        }
    }

    start_server {} {
        set replica1 [srv 0 client]
        start_server {} {
            set replica2 [srv 0 client]

            test "Benchmark standalone replica sync under pipeline load with QoS event loop" {
                set load_clients {}
                for {set c 0} {$c < 5} {incr c} {
                    lappend load_clients [valkey $primary_host $primary_port 0 $::tls]
                }

                set val [string repeat "x" 256]
                set pipeline ""
                for {set p 0} {$p < 20} {incr p} {
                    append pipeline "*3\r\n\$3\r\nSET\r\n\$7\r\npipekey\r\n\$256\r\n$val\r\n"
                }

                $replica1 replicaof $primary_host $primary_port
                $replica2 replicaof $primary_host $primary_port

                set total_ops 0
                for {set iter 0} {$iter < 10} {incr iter} {
                    foreach cl $load_clients {
                        catch {
                            $cl write $pipeline
                            $cl flush
                            incr total_ops 20
                            for {set r 0} {$r < 20} {incr r} { $cl read }
                        }
                    }
                }

                wait_for_condition 100 50 {
                    [status $primary connected_slaves] == 2 &&
                    [$replica1 dbsize] == 1000 &&
                    [$replica2 dbsize] == 1000
                } else {
                    fail "Replicas failed to complete sync during pipelined load"
                }

                foreach cl $load_clients { $cl close }
            }
        }

        test {Dynamic configuration of qos-preemptive-poll-interval-us} {
            assert_equal [lindex [r config get qos-preemptive-poll-interval-us] 1] 2000
            r config set qos-preemptive-poll-interval-us 500
            assert_equal [lindex [r config get qos-preemptive-poll-interval-us] 1] 500
            r config set qos-preemptive-poll-interval-us 0
            assert_equal [lindex [r config get qos-preemptive-poll-interval-us] 1] 0
            assert_error "*argument must be between*" {r config set qos-preemptive-poll-interval-us -1}
            assert_error "*argument couldn't be parsed into an integer*" {r config set qos-preemptive-poll-interval-us invalid}
            r config set qos-preemptive-poll-interval-us 2000
            assert_equal [lindex [r config get qos-preemptive-poll-interval-us] 1] 2000
        }

        test {Socket Prioritization: INFO stats and debug QoS metrics} {
            set info_stats [$primary info stats]
            set info_debug [$primary info debug]

            assert_morethan [getInfoProperty $info_stats qos_eventloop_cycles] 0
            assert_morethan [getInfoProperty $info_stats qos_eventloop_duration_sum] 0
            assert_morethan [getInfoProperty $info_stats qos_eventloop_duration_cmd_sum] 0
            assert {[getInfoProperty $info_debug qos_eventloop_duration_max] >= 0}
            assert {[getInfoProperty $info_debug qos_eventloop_cmd_per_cycle_max] >= 0}

            # Reset stats and verify
            $primary config resetstat
            set info_stats_reset [$primary info stats]
            assert_equal [getInfoProperty $info_stats_reset qos_eventloop_cycles] 0
            assert_equal [getInfoProperty $info_stats_reset qos_eventloop_duration_sum] 0
            assert_equal [getInfoProperty $info_stats_reset qos_eventloop_duration_cmd_sum] 0
            assert_equal [getInfoProperty [$primary info debug] qos_eventloop_cmd_per_cycle_max] 0
        }
    }
}
