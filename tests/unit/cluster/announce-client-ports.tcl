# Small cluster. No need for failovers.
start_cluster 2 2 {tags {external:skip cluster} overrides {cluster-replica-no-failover yes}} {

    test "Set cluster announced client (TLS) port and check that it propagates" {
        set announced_ports {}
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            set res [R $j config set cluster-announce-client-port "640$j"]
            set res [R $j config set cluster-announce-client-tls-port "640$j"]
            lappend announced_ports "640$j"
        }

        # CLUSTER SLOTS
        wait_for_condition 50 100 {
            [are_cluster_announced_values_propagated "port" $announced_ports]
        } else {
            fail "cluster-announce-client-(tls-)port were not propagated"
        }

        # CLUSTER SHARDS
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            foreach shard [R $j CLUSTER SHARDS] {
                foreach node [dict get $shard "nodes"] {
                    set port [dict get $node "port"]
                    assert_match "640*" $port

                    if {$::tls} {
                        set tls_port [dict get $node "tls-port"]
                        assert_match "640*" $tls_port
                    }
                }
            }
        }

        # CLUSTER NODES
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            set lines [split [R $j CLUSTER NODES] "\r\n"]
            foreach l $lines {
                set l [string trim $l]
                if {$l eq {}} continue
                assert_equal 1 [regexp {^[0-9a-f]+ 127\.0\.0\.1:640[0-9]@} $l]
            }
        }

        # Redirects
        catch {R 0 set foo foo} e
        assert_match "MOVED * *:640*" $e

        # Now that everything is propagated, assert everyone agrees
        wait_for_cluster_propagation
    }

    test "Clear announced client (TLS) port and check that it propagates" {
        set original_ports {}
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            R $j config set cluster-announce-client-port 0
            R $j config set cluster-announce-client-tls-port 0
            if {$::tls} {
                lappend original_ports [lindex [R $j config get tls-port] 1]
            } else {
                lappend original_ports [lindex [R $j config get port] 1]
            }
        }

        wait_for_condition 50 100 {
            [are_cluster_announced_values_propagated "port" $original_ports] eq 1
        } else {
            fail "Cleared cluster-announce-client-(tls-)port were not propagated"
        }

        # Redirect uses the original port
        catch {R 0 set foo foo} e
        assert_match "MOVED * *:*" $e
        set dest_port [lindex [split $e ":"] end]
        assert {[lsearch -exact $original_ports $dest_port] >= 0}

        # Now that everything is propagated, assert everyone agrees
        wait_for_cluster_propagation
    }
}
