# Tests for cluster-resolve-hostnames, which lets nodes recover from stale IPs
# in nodes.conf by resolving the announced hostnames of the nodes.

# An address from TEST-NET-1 (RFC 5737) that nothing listens on.
set ::stale_ip "192.0.2.1"

start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-replica-no-failover yes cluster-announce-hostname localhost cluster-resolve-hostnames yes}} {
    test "Cluster recovers after a full restart with stale IPs in nodes.conf" {
        wait_for_cluster_propagation
        set num_nodes [llength $::servers]

        # Shut down every node, then rewrite all addresses in nodes.conf to the
        # stale IP, as if every node came back with a new IP after a restart.
        set conf_paths {}
        for {set j 0} {$j < $num_nodes} {incr j} {
            lappend conf_paths [cluster_nodes_conf_path $j]
        }
        for {set j 0} {$j < $num_nodes} {incr j} {
            catch {R $j shutdown nosave}
        }
        foreach path $conf_paths {
            set content [read_file $path]
            regsub -all {127\.0\.0\.1:} $content "$::stale_ip:" content
            write_file $path $content
        }

        for {set j 0} {$j < $num_nodes} {incr j} {
            restart_server -$j true false
        }

        wait_for_cluster_state ok
        wait_for_condition 1000 50 {
            ![string match "*$::stale_ip*" [R 0 cluster nodes]] &&
            ![string match "*$::stale_ip*" [R 1 cluster nodes]] &&
            ![string match "*$::stale_ip*" [R 2 cluster nodes]] &&
            ![string match "*$::stale_ip*" [R 3 cluster nodes]] &&
            ![string match "*$::stale_ip*" [R 4 cluster nodes]] &&
            ![string match "*$::stale_ip*" [R 5 cluster nodes]]
        } else {
            fail "Stale IPs were not replaced by the resolved addresses"
        }
        wait_for_cluster_propagation

        # Replicas must follow the resolved address of their primary.
        for {set j 3} {$j < $num_nodes} {incr j} {
            wait_for_condition 1000 50 {
                [s -$j master_link_status] eq {up}
            } else {
                fail "Replica $j did not reconnect to its primary"
            }
        }
    }
}

start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-replica-no-failover yes cluster-announce-hostname localhost cluster-resolve-hostnames yes}} {
    test "Node that changed its ID under the same hostname stays NOADDR" {
        set primary0_id [R 0 CLUSTER MYID]

        # After CLUSTER RESET HARD, R 0 answers on the same address and hostname
        # with a new node ID, so the other nodes mark the old ID as NOADDR.
        R 0 cluster reset hard
        wait_for_log_messages -1 {"*PONG contains mismatching sender ID*"} 0 1000 10
        wait_for_log_messages -2 {"*PONG contains mismatching sender ID*"} 0 1000 10
        wait_for_condition 1000 10 {
            [cluster_has_flag [cluster_get_node_by_id 1 $primary0_id] noaddr] eq 1 &&
            [cluster_has_flag [cluster_get_node_by_id 2 $primary0_id] noaddr] eq 1
        } else {
            fail "The node is not marked as NOADDR"
        }

        # The hostname still resolves to the address that answered with the new
        # ID, so it must not be used to restore the old entry. Give it several
        # resolve intervals and check the mismatch does not repeat.
        set mismatches1 [count_message_lines [srv -1 stdout] "PONG contains mismatching sender ID"]
        set mismatches2 [count_message_lines [srv -2 stdout] "PONG contains mismatching sender ID"]
        after 3500
        assert_equal $mismatches1 [count_message_lines [srv -1 stdout] "PONG contains mismatching sender ID"]
        assert_equal $mismatches2 [count_message_lines [srv -2 stdout] "PONG contains mismatching sender ID"]
        assert_equal 1 [cluster_has_flag [cluster_get_node_by_id 1 $primary0_id] noaddr]
        assert_equal 1 [cluster_has_flag [cluster_get_node_by_id 2 $primary0_id] noaddr]
    }
}

start_cluster 3 0 {tags {external:skip cluster} overrides {cluster-resolve-hostnames yes cluster-node-timeout 3000}} {
    test "Unresolvable hostname does not block the node and keeps the last known address" {
        set R1_id [R 1 cluster myid]
        R 1 config set cluster-announce-hostname "unresolvable.invalid"
        wait_for_condition 50 100 {
            [string match "*,unresolvable.invalid*" [dict get [cluster_get_node_by_id 0 $R1_id] addr]]
        } else {
            fail "Hostname of R1 was not propagated"
        }
        set addr [dict get [cluster_get_node_by_id 0 $R1_id] addr]

        # Make R1 unreachable so R0 tries to resolve its hostname.
        set loglines [count_log_lines 0]
        pause_process [srv -1 pid]
        wait_for_log_messages 0 {"*Unable to resolve hostname unresolvable.invalid*"} $loglines 300 100

        # R0 keeps serving and still uses the last known address of R1.
        assert_equal PONG [R 0 ping]
        assert_equal $addr [dict get [cluster_get_node_by_id 0 $R1_id] addr]

        resume_process [srv -1 pid]
        wait_for_cluster_state ok
    }

    test "Own hostname is resolved again after the address config changes" {
        set R2_id [R 2 cluster myid]

        # Let R2 resolve its own hostname once.
        R 2 config set cluster-announce-hostname localhost
        after 2000

        # A new hostname must be looked up again.
        set loglines [count_log_lines -2]
        R 2 config set cluster-announce-hostname "unresolvable.invalid"
        wait_for_log_messages -2 [list "*Unable to resolve hostname unresolvable.invalid of node $R2_id*"] $loglines 300 100

        # So must the hostname after cluster-announce-ip is removed.
        R 2 config set cluster-announce-ip 127.0.0.1
        set loglines [count_log_lines -2]
        R 2 config set cluster-announce-ip ""
        wait_for_log_messages -2 [list "*Unable to resolve hostname unresolvable.invalid of node $R2_id*"] $loglines 300 100

        R 2 config set cluster-announce-hostname ""
    }
}
