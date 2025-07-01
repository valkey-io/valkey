start_server {tags {needs:repl external:skip "quit"}} {
    start_server {} {
        set node_0 [srv -1 client]
        set node_0_host [srv -1 host]
        set node_0_port [srv -1 port]

        set node_1_host [srv 0 host]
        set node_1_port [srv 0 port]

        test {basic failover with tunnel} {
            # Change the current instance to be a replica
            r REPLICAOF $node_0_host $node_0_port
            wait_replica_online $node_0
            $node_0 FAILOVER TO $node_1_host $node_1_port TIMEOUT 100 FORCE TUNNEL

            # Wait for failover completion
            wait_for_condition 50 100 {
                [s -1 master_failover_state] == "no-failover"
            } else {
                fail "Failover timed-out"
            }
        }
        test {perform tunneling} {
            set replica [valkey_client -1]
            set value_str [string repeat "A" 16000]
            set client_sock [socket -myaddr 127.0.0.2 $node_0_host $node_0_port]
            puts $client_sock "SET foo $value_str"
            flush $client_sock
            gets $client_sock line
            assert_match "+OK" $line
            puts $client_sock "GET foo"
            flush $client_sock
            gets $client_sock line
            assert_match "\$16000" $line
            gets $client_sock line
            assert_match $value_str $line
            close $client_sock
        }

        test {failover back to the original primary without tunneling} {
            set primary [valkey_client]
            $primary FAILOVER TO $node_0_host $node_0_port TIMEOUT 100 FORCE

            # Wait for failover completion
            wait_for_condition 50 100 {
                [s -1 master_failover_state] == "no-failover"
            } else {
                fail "Failover timed-out"
            }
            $primary close
            set replica [valkey_client]
            assert_error "READONLY*" {$replica SET foo boo}
            $replica close
        }
        test {avoid tunneling if client excluded ip} {
            set primary [valkey_client -1]
            $primary FAILOVER TO $node_1_host $node_1_port TIMEOUT 100 FORCE TUNNEL 127.0.0.3

            # Wait for failover completion
            wait_for_condition 50 100 {
                [s -1 master_failover_state] == "no-failover"
            } else {
                fail "Failover timed-out"
            }
            set client_sock [socket -myaddr 127.0.0.3 $node_0_host $node_0_port]
            set value_str [string repeat "A" 16000]
            puts $client_sock "SET foo $value_str"
            flush $client_sock
            gets $client_sock line
            assert_match "-READONLY*" $line
            close $client_sock
            $primary close
        }
    }
}

