# Check if cluster's view of human announced nodename is reported in logs

# Override cluster-node-timeout: shorten timeout to quickly trigger the failure message
# Override loglevel: we need to use some debug level logs in assertions
# Override cluster-announce-human-nodename: cluster nodes in test suite are assigned human nodenames
#       like R0, R1, R2 etc. So we temporarily turn off the setting so that we can run our test cases here.
start_cluster 4 0 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 loglevel "debug" cluster-announce-human-nodename "''"}} {
    set RO_node_id [dict get [cluster_get_myself 0] id]
    set R0_port [srv 0 port]
    set R1_port [srv -1 port]
    set R2_port [srv -2 port]
    set R3_port [srv -3 port]

    test "Use ip:port in logging when human nodenames are not explicitly set" {
        wait_for_log_messages 0 [list "*Sending ping packet to node * (127.0.0.1:$R1_port) *"] 0 1000 10
        wait_for_log_messages 0 [list "*Sending ping packet to node * (127.0.0.1:$R2_port) *"] 0 1000 10
        wait_for_log_messages 0 [list "*Sending ping packet to node * (127.0.0.1:$R3_port) *"] 0 1000 10
        wait_for_log_messages -1 [list "*Sending ping packet to node $RO_node_id (127.0.0.1:$R0_port) *"] 0 1000 10
        wait_for_log_messages -2 [list "*Sending ping packet to node $RO_node_id (127.0.0.1:$R0_port) *"] 0 1000 10
        wait_for_log_messages -3 [list "*Sending ping packet to node $RO_node_id (127.0.0.1:$R0_port) *"] 0 1000 10
    }

    test "The helper function humanNodename works when a single logging statement contains multiple calls" {
        # Pause two instances, so the other two instances report these node failures
        pause_process [srv 0 pid]
        pause_process [srv -1 pid]

        # There are many logging statements that contain two calls to helper function `humanNodename`.
        # We pick the "reported node .. as not reachable" logging here because it's easy to trigger.
        wait_for_log_messages -2 [list "*Node * (127.0.0.1:$R3_port) reported node $RO_node_id (127.0.0.1:$R0_port) as not reachable*"] 0 200 50
        wait_for_log_messages -3 [list "*Node * (127.0.0.1:$R2_port) reported node $RO_node_id (127.0.0.1:$R0_port) as not reachable*"] 0 200 50

        resume_process [srv 0 pid]
        resume_process [srv -1 pid]
    }

    test "Set cluster human announced nodename and let it propagate" {
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            R $j config set cluster-announce-hostname "host-$j.com"
            R $j config set cluster-announce-human-nodename "nodename-$j"
        }

        # We wait for everyone to agree on the hostnames. Since they are gossiped
        # the same way as nodenames, it implies everyone knows the nodenames too.
        wait_for_condition 50 100 {
            [are_hostnames_propagated "host-*.com"] eq 1
        } else {
            fail "cluster hostnames were not propagated"
        }

        # Ensure the human nodenames are visible in logs
        wait_for_log_messages 0 [list "*Sending ping packet to node * (nodename-1) *"] 0 1000 10
        wait_for_log_messages 0 [list "*Sending ping packet to node * (nodename-2) *"] 0 1000 10
        wait_for_log_messages 0 [list "*Sending ping packet to node * (nodename-3) *"] 0 1000 10
        wait_for_log_messages -1 [list "*Sending ping packet to node $RO_node_id (nodename-0) *"] 0 1000 10
        wait_for_log_messages -2 [list "*Sending ping packet to node $RO_node_id (nodename-0) *"] 0 1000 10
        wait_for_log_messages -3 [list "*Sending ping packet to node $RO_node_id (nodename-0) *"] 0 1000 10
    }
}

start_cluster 4 0 {tags {external:skip cluster ipv6} overrides {bind {127.0.0.1 ::1} cluster-announce-ip ::1 loglevel "debug" cluster-announce-human-nodename "''"}} {
    set RO_node_id [dict get [cluster_get_myself 0] id]
    set R0_port [srv 0 port]
    set R1_port [srv -1 port]
    set R2_port [srv -2 port]
    set R3_port [srv -3 port]

    test "ip:port as nodename in logging also works for IPv6" {
        wait_for_log_messages 0 [list "*Sending ping packet to node * (\\\[::1\\\]:$R1_port) *"] 0 1000 10
        wait_for_log_messages 0 [list "*Sending ping packet to node * (\\\[::1\\\]:$R2_port) *"] 0 1000 10
        wait_for_log_messages 0 [list "*Sending ping packet to node * (\\\[::1\\\]:$R3_port) *"] 0 1000 10
        wait_for_log_messages -1 [list "*Sending ping packet to node $RO_node_id (\\\[::1\\\]:$R0_port) *"] 0 1000 10
        wait_for_log_messages -2 [list "*Sending ping packet to node $RO_node_id (\\\[::1\\\]:$R0_port) *"] 0 1000 10
        wait_for_log_messages -3 [list "*Sending ping packet to node $RO_node_id (\\\[::1\\\]:$R0_port) *"] 0 1000 10
    }
}