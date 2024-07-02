# Start a cluster with 3 masters and 4 replicas.
# These tests rely on specific node ordering, so make sure no node fails over.
start_cluster 2 2 {tags {external:skip cluster} overrides {cluster-replica-no-failover yes bind {127.0.0.1 ::1}}} {

    test "Set cluster announced IPv4 and check that it propagates" {
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            set res [R $j config set cluster-announce-client-ipv4 "111.222.111.$j"]
            puts "CONFIG SET for node $j: $res"
        }

        # CLUSTER SLOTS
        wait_for_condition 50 100 {
            [are_cluster_announced_ips_propagated {111.222.111.*}]
        } else {
            fail "cluster-announce-client-ipv4 were not propagated"
        }

        # CLUSTER SHARDS
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            foreach shard [R $j CLUSTER SHARDS] {
                foreach node [dict get $shard "nodes"] {
                    set ip [dict get $node "ip"]
                    set endpoint [dict get $node "endpoint"]
                    assert_match "111.222.111*" $ip
                    assert_match "111.222.111*" $endpoint
                }
            }
        }

        # CLUSTER NODES
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            set lines [split [R $j CLUSTER NODES] "\r\n"]
            foreach l $lines {
                set l [string trim $l]
                if {$l eq {}} continue
                assert_equal 1 [regexp {^[0-9a-f]+ 111\.222\.111\.[0-9]} $l]
            }
        }

        # Redirects
        catch {R 0 set foo foo} e
        assert_match "MOVED * 111.222.111*:*" $e

        # Now that everything is propagated, assert everyone agrees
        wait_for_cluster_propagation
    }

    test "Set cluster announced IPv6 and check that it propagates" {
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            R $j config set cluster-announce-client-ipv6 "cafe:1234::$j"
        }

        # Connect clients to localhost as "::1" makes them use IPv6.
        set clients {}
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            set level [expr -1 * $j]
            lappend clients [valkey ::1 [srv $level port] 0 $::tls]
        }

        # CLUSTER SLOTS
        wait_for_condition 50 100 {
            [are_cluster_announced_ips_propagated "cafe:1234::*" $clients] eq 1
        } else {
            fail "cluster-announce-client-ipv6 were not propagated"
        }

        # CLUSTER SHARDS
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            foreach shard [[lindex $clients $j] CLUSTER SHARDS] {
                foreach node [dict get $shard "nodes"] {
                    set ip [dict get $node "ip"]
                    set endpoint [dict get $node "endpoint"]
                    assert_match "cafe:1234::*" $ip
                    assert_match "cafe:1234::*" $endpoint
                }
            }
        }

        # CLUSTER NODES
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            set lines [split [[lindex $clients $j] CLUSTER NODES] "\r\n"]
            foreach l $lines {
                set l [string trim $l]
                if {$l eq {}} continue
                assert_equal 1 [regexp {^[0-9a-f]+ cafe:1234::[0-9]} $l]
            }
        }

        # Redirects
        catch {[lindex $clients 0] set foo foo} e
        assert_match "MOVED * cafe:1234::*:*" $e

        # Close clients
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            [lindex $clients $j] close
        }

        # Now that everything is propagated, assert everyone agrees
        wait_for_cluster_propagation
    }

    test "Clear announced client IPv4 and check that it propagates" {
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            R $j config set cluster-announce-client-ipv4 ""
        }

        wait_for_condition 50 100 {
            [are_cluster_announced_ips_propagated "127.0.0.1"] eq 1
        } else {
            fail "Cleared cluster-announce-client-ipv4 were not propagated"
        }

        # Redirect use the IP address
        catch {R 0 set foo foo} e
        assert_match "MOVED * 127.0.0.1:*" $e

        # Now that everything is propagated, assert everyone agrees
        wait_for_cluster_propagation
    }

    test "Clear announced client IPv6 and check that it propagates" {
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            R $j config set cluster-announce-client-ipv6 ""
        }

        # Connect clients to localhost as "::1" makes them use IPv6.
        set clients {}
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            set level [expr -1 * $j]
            lappend clients [valkey ::1 [srv $level port] 0 $::tls]
        }

        wait_for_condition 50 100 {
            [are_cluster_announced_ips_propagated "127.0.0.1" $clients] eq 1
        } else {
            fail "Cleared cluster-announce-client-ipv6 were not propagated"
        }

        # Redirect use the IP address
        catch {[lindex $clients 0] set foo foo} e
        assert_match "MOVED * 127.0.0.1:*" $e

        # Close clients
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            [lindex $clients $j] close
        }

        # Now that everything is propagated, assert everyone agrees
        wait_for_cluster_propagation
    }
}
