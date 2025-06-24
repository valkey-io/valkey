start_server {tags {needs:repl external:skip "quit"}} {
    start_server {} {
        set node_0 [srv -1 client]
        set node_0_host [srv -1 host]
        set node_0_port [srv -1 port]
        set node_0_pid [srv -1 pid]

        set node_1 [srv 0 client]
        set node_1_host [srv 0 host]
        set node_1_port [srv 0 port]
        set node_1_pid [srv 0 pid]

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
             # Generate a large string
            set value_str [string repeat "A" 512000]
            $node_0 SET foo $value_str
            assert_match $value_str [$node_0 get foo]
        }

        test {REDIRECT takes president over tunnel} {
            $node_1 CLIENT CAPA REDIRECT
            $node_1 FAILOVER TO $node_0_host $node_0_port TIMEOUT 100 FORCE TUNNEL
            # Wait for failover completion
            wait_for_condition 50 100 {
                [s master_failover_state] == "no-failover"
            } else {
                fail "Failover timed-out"
            }
            assert_error "REDIRECT $node_0_host:$node_0_port" {$node_1 set foo bar}
            assert_error "REDIRECT $node_0_host:$node_0_port" {$node_1 get foo}
        }
        test {previous tunnel flag shouldn't pesists} {
            set rd [valkey_client -1]
            $rd FAILOVER TO $node_1_host $node_1_port TIMEOUT 100 FORCE

            # Wait for failover completion
            wait_for_condition 50 100 {
                [s -1 master_failover_state] == "no-failover"
            } else {
                fail "Failover timed-out"
            }
            assert_error {READONLY*} {$rd SET foo bar}
            $rd close
        }
        test {tunnel with MULTI} {
            set rr [valkey_client]
            set rr1 [valkey_client]
            $rr MULTI
            assert_equal "QUEUED" [$rr SET foo bar]
            $rr1 FAILOVER TO $node_0_host $node_0_port TIMEOUT 100 FORCE TUNNEL

            # Wait for failover completion
            wait_for_condition 50 100 {
                [s master_failover_state] == "no-failover"
            } else {
                fail "Failover timed-out"
            }
            assert_equal "QUEUED" [$rr GET foo]
            assert_equal "QUEUED" [$rr SET foo1 bar1]
            assert_equal "QUEUED" [$rr GET foo1]
            assert_equal "OK bar OK bar1" [$rr EXEC]
            $rr close
            $rr1 close
        }

    }
}

