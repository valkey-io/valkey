# Test a single primary can mark replica as `fail`
start_cluster 1 1 {tags {external:skip cluster}} {

    test "Verify that single primary marks replica as failed" {
        set primary [srv -0 client]

        set replica1 [srv -1 client]
        set replica1_pid [srv -1 pid]
        set replica1_instance_id [dict get [cluster_get_myself 1] id]

        assert {[lindex [$primary role] 0] eq {master}}
        assert {[lindex [$replica1 role] 0] eq {slave}}

        wait_for_sync $replica1

        pause_process $replica1_pid

        wait_node_marked_fail 0 $replica1_instance_id

        resume_process $replica1_pid
    }
}

# Test multiple primaries wait for a quorum and then mark a replica as `fail`
start_cluster 2 1 {tags {external:skip cluster}} {

    test "Verify that multiple primaries mark replica as failed" {
        set primary1 [srv -0 client]

        set primary2 [srv -1 client]
        set primary2_pid [srv -1 pid]

        set replica1 [srv -2 client]
        set replica1_pid [srv -2 pid]
        set replica1_instance_id [dict get [cluster_get_myself 2] id]

        assert {[lindex [$primary1 role] 0] eq {master}}
        assert {[lindex [$primary2 role] 0] eq {master}}
        assert {[lindex [$replica1 role] 0] eq {slave}}

        wait_for_sync $replica1

        pause_process $replica1_pid

        # Pause other primary to allow time for pfail flag to appear
        pause_process $primary2_pid

        wait_node_marked_pfail 0 $replica1_instance_id

        # Resume other primary and wait for to show replica as failed
        resume_process $primary2_pid

        wait_node_marked_fail 0 $replica1_instance_id

        resume_process $replica1_pid
    }
}

set old_singledb $::singledb
set ::singledb 1

tags {external:skip tls:skip cluster} {
    set base_conf [list cluster-enabled yes cluster-ping-interval 100 cluster-node-timeout 3000 save ""]
    start_multiple_servers 5 [list overrides $base_conf] {
        test "Only primary with slots has the right to mark a node as failed" {
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]
            set primary_pid [srv 0 pid]
            set primary_id [R 0 CLUSTER MYID]
            set replica_id [R 1 CLUSTER MYID]
            set replica_pid [srv -1 pid]

            # Meet others nodes.
            R 1 CLUSTER MEET $primary_host $primary_port
            R 2 CLUSTER MEET $primary_host $primary_port
            R 3 CLUSTER MEET $primary_host $primary_port
            R 4 CLUSTER MEET $primary_host $primary_port

            # Build a single primary cluster.
            cluster_allocate_slots 1 1
            wait_for_cluster_propagation
            R 1 CLUSTER REPLICATE $primary_id
            wait_for_cluster_propagation
            wait_for_cluster_state "ok"

            # Pause the primary, marking the primary as pfail.
            pause_process $primary_pid
            wait_node_marked_pfail 1 $primary_id
            wait_node_marked_pfail 2 $primary_id
            wait_node_marked_pfail 3 $primary_id
            wait_node_marked_pfail 4 $primary_id

            # Pause the replica, marking the replica as pfail.
            pause_process $replica_pid
            wait_node_marked_pfail 2 $replica_id
            wait_node_marked_pfail 3 $replica_id
            wait_node_marked_pfail 4 $replica_id

            # Resume the primary, marking the replica as fail.
            resume_process $primary_pid
            wait_node_marked_fail 0 $replica_id
            wait_node_marked_fail 2 $replica_id
            wait_node_marked_fail 3 $replica_id
            wait_node_marked_fail 4 $replica_id

            # Check if we got the right failure reports.
            wait_for_condition 1000 50 {
                [R 0 CLUSTER COUNT-FAILURE-REPORTS $replica_id] == 0 &&
                [R 2 CLUSTER COUNT-FAILURE-REPORTS $replica_id] == 1 &&
                [R 3 CLUSTER COUNT-FAILURE-REPORTS $replica_id] == 1 &&
                [R 4 CLUSTER COUNT-FAILURE-REPORTS $replica_id] == 1
            } else {
                fail "Cluster COUNT-FAILURE-REPORTS is not right."
            }

            resume_process $replica_pid
        }
    }
}

set ::singledb $old_singledb
