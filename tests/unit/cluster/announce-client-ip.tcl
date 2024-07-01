# Start a cluster with 3 masters and 4 replicas.
# These tests rely on specific node ordering, so make sure no node fails over.
start_cluster 4 4 {tags {external:skip cluster ipv6} overrides {cluster-replica-no-failover yes}} {

    test "Set cluster announced IPv4 and chec that it propagates" {
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            R $j config set cluster-announce-client-ipv4 "111.222.111.$j"
        }

        wait_for_condition 50 100 {
            [are_cluster_announced_ips_propagated "111.222.111.*"] eq 1
        } else {
            fail "cluster-announce-client-ipv4 were not propagated"
        }

        # Redirects use the IP address
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
        array set clients {}
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            set level [expr -1 * $j]
            set clients($j) [valkey ::1 [srv $level port] 0 $::tls]
        }

        wait_for_condition 50 100 {
            [are_cluster_announced_ips_propagated "cafe:1234::*" $clients] eq 1
        } else {
            fail "cluster-announce-client-ipv6 were not propagated"
        }

        # Redirect use the IP address
        catch {$clients(0) set foo foo} e
        assert_match "MOVED * cafe:1234::*:*" $e

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
        catch {$clients(0) set foo foo} e
        assert_match "MOVED * 127.0.0.1:*" $e

        # Now that everything is propagated, assert everyone agrees
        wait_for_cluster_propagation
    }

    test "Clear announced client IPv6 and check that it propagates" {

        for {set j 0} {$j < [llength $::servers]} {incr j} {
            R $j config set cluster-announce-client-ipv6 "cafe:1234::$j"
        }

        # Connect clients to localhost as "::1" makes them use IPv6.
        array set clients {}
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            set level [expr -1 * $j]
            set clients($j) [valkey ::1 [srv $level port] 0 $::tls]
        }

        wait_for_condition 50 100 {
            [are_cluster_announced_ips_propagated "127.0.0.1" $clients] eq 1
        } else {
            fail "Cleared cluster-announce-client-ipv6 were not propagated"
        }

        # Redirect use the IP address
        catch {$clients(0) set foo foo} e
        assert_match "MOVED * 127.0.0.1:*" $e

        # Now that everything is propagated, assert everyone agrees
        wait_for_cluster_propagation
    }
}
