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

        test {The TUNNEL command can not be issued against a replica node} {
            set replica [valkey_client -1]
            assert_error {*TUNNEL cannot be used with replica instances} {$replica TUNNEL 2 USER default}
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

        test {The TUNNEL command uses invalid resp version} {
            set primary [valkey_client -1]
            assert_error {ERR Unsupported resp protocol version} {$primary TUNNEL 5}
            $primary close
        }
        test {The TUNNEL command uses invalid resp version 1} {
            set primary [valkey_client -1]
            assert_error {ERR Protocol version is not an integer or out of range} {$primary TUNNEL a}
            $primary close
        }
        test {The TUNNEL command requires authentication params if primaryauth is configured on the primary} {
            set primary [valkey_client -1]
            $primary config set primaryauth "abc"
            $primary ACL SETUSER user_1 on >pwd_1 allcommands allkeys -@admin
            $primary ACL SETUSER user_2 on >pwd_1 allcommands allkeys +@admin
            $primary AUTH user_1 pwd_1
            assert_error {*NOPERM User user_1 has no permissions to run the 'tunnel'*} {$primary TUNNEL 2}
            $primary close
        }
        test {The TUNNEL command requires auth user if primaryuser is configured on the primary} {
            set primary [valkey_client -1]
            $primary AUTH user_2 pwd_1
            $primary config set primaryuser "user_1"
            $primary AUTH user_1 pwd_1
            assert_error {*NOPERM User user_1 has no permissions to run the 'tunnel'*} {$primary TUNNEL 2}
            $primary close
        }
        test {Auth the client as admin before issue the TUNNEL command} {
            set primary [valkey_client -1]
            $primary AUTH user_2 pwd_1
            $primary TUNNEL 2 USER default
            $primary close
        }
        test {Using valid primary auth info for the upstream tunnel connection} {
            set replica [valkey_client]
            $replica ACL SETUSER user_1 on >abc allcommands allkeys +@admin
            set primary [valkey_client -1]
            $primary AUTH user_2 pwd_1
            $replica ACL SETUSER user_2 on >pwd_1 allcommands allkeys
            $primary FAILOVER TO $node_1_host $node_1_port TIMEOUT 100 FORCE TUNNEL

            # Wait for failover completion
            wait_for_condition 50 100 {
                [s -1 master_failover_state] == "no-failover"
            } else {
                fail "Failover timed-out"
            }
             # Generate a large string
            set value_str [string repeat "A" 512000]
            $primary SET foo $value_str
            $primary close
            $replica close
        }
        test {Failing the upstream tunnel connection due to client username ACL restriction} {
            set primary [valkey_client]
            $primary ACL SETUSER user_3 on >pwd_3 allcommands -SET allkeys
            set replica [valkey_client -1]
            $replica ACL SETUSER user_3 on >pwd_3 allcommands
            $replica AUTH user_3 pwd_3
            assert_error {NOPERM No permissions to access a key} {$replica SET foo bar}
            $primary close
            $replica close
        }
        test {succeeding the upstream tunnel connection due to client username ACL restriction} {
            set primary [valkey_client]
            set replica [valkey_client -1]
            $primary ACL SETUSER user_4 on >pwd_4 allcommands allkeys
            $replica ACL SETUSER user_4 on >pwd_4 allcommands allkeys
            $replica AUTH user_4 pwd_4
            $replica SET foo bar
            $primary close
            $replica close
        }
    }
}

