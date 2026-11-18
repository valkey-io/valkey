# The best ranked replica sets FORCEACK on its election request, so that the
# primaries vote for it even if they did not receive the FAIL message yet. That
# is only needed right after the replica marked its primary as FAIL. Later on,
# a primary that can reach the failed primary again has cleared the FAIL flag,
# and a replica that still cannot reach it must not override that.
#
# We have the following nodes:
# Primary: R0  R1  R2
# Replica: R3 (replica of R0)
#
# R0 owns slots 0-5461, key key_977613 belongs to slot 0.
#
# R0 is paused until R1, R2 and R3 mark it as FAIL. R3 is the best ranked
# replica and starts an election right away. R1 votes for it, R2 and R0 drop
# the request, so the election ends with one of the two votes it needs. R0 then
# resumes. R1 and R2 clear its FAIL flag, but R3 never reconnects to R0 and
# keeps the flag. The retried election of R3 must be refused by both primaries.
start_cluster 3 1 {tags {external:skip cluster} overrides {cluster-ping-interval 100 cluster-node-timeout 2000 cluster-replica-validity-factor 0}} {
    test "A retried election does not override primaries that see the failed primary as up" {
        set CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST 5
        set R0_nodeid [R 0 cluster myid]
        set R1_nodeid [R 1 cluster myid]

        # Write some data to R0 so that R3 has a non-zero offset.
        for {set i 0} {$i < 10} {incr i} {
            R 0 incr key_977613
        }
        wait_for_ofs_sync [srv 0 client] [srv -3 client]

        # R0 and R2 do not answer the first election of R3.
        R 0 debug drop-cluster-packet-filter $CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST
        R 2 debug drop-cluster-packet-filter $CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST

        set loglines2 [count_log_lines -2]
        set loglines3 [count_log_lines -3]
        pause_process [srv 0 pid]
        wait_for_condition 1000 50 {
            [cluster_all_see_flag {1 2 3} [list $R0_nodeid] fail]
        } else {
            fail "R1, R2 and R3 did not mark R0 as FAIL"
        }

        # R3 starts the election immediately and only R1 votes for it.
        wait_for_log_messages -3 {"*This is the best ranked replica and can initiate the election immediately*"} $loglines3 1000 50
        wait_for_log_messages -3 [list "*Failover auth ACK from $R1_nodeid * (ACKs 1, quorum 2)*"] $loglines3 1000 50

        # R2 has dropped that request, so it can answer the next one.
        wait_for_log_messages -2 {"*Dropping packet of type auth-req*"} $loglines2 1000 50
        R 2 debug drop-cluster-packet-filter -1

        # R3 cannot reach R0 again, even after R0 resumes.
        R 3 debug disable-cluster-reconnection 1
        R 3 debug clusterlink kill all $R0_nodeid
        resume_process [srv 0 pid]

        # R1 and R2 reach R0 again and clear its FAIL flag, R3 keeps it.
        wait_for_condition 1000 50 {
            ![cluster_has_flag [cluster_get_node_by_id 1 $R0_nodeid] fail] &&
            ![cluster_has_flag [cluster_get_node_by_id 2 $R0_nodeid] fail]
        } else {
            fail "R1 and R2 did not clear the FAIL flag of R0"
        }
        assert {[cluster_has_flag [cluster_get_node_by_id 3 $R0_nodeid] fail]}

        # R3 retries 4 times cluster-node-timeout after its first election. If it already
        # retried, R1 and R2 may have seen R0 as failed, so the run was too slow for this
        # test and a grant below would not point at the FORCEACK bug.
        if {[count_log_message -3 "Starting a failover election"] != 1} {
            fail "R3 retried its election before R1 and R2 cleared the FAIL flag of R0"
        }

        # R1 and R2 both see R0 as up and must refuse.
        set loglines1 [count_log_lines -1]
        set loglines2 [count_log_lines -2]
        foreach {idx loglines} [list 1 $loglines1 2 $loglines2] {
            set res [wait_for_log_messages -$idx {"*Failover auth granted to*" "*Failover auth denied to*"} $loglines 1000 50]
            assert_match "*Failover auth denied to*its primary is up*" [lindex $res 0]
        }
        assert_equal "slave" [s -3 role]
        assert_equal "master" [s 0 role]

        R 0 debug drop-cluster-packet-filter -1
        R 3 debug disable-cluster-reconnection 0
        wait_for_condition 1000 50 {
            ![cluster_has_flag [cluster_get_node_by_id 3 $R0_nodeid] fail]
        } else {
            fail "R3 did not clear the FAIL flag of R0"
        }
        wait_for_cluster_state ok
    }
} ;# start_cluster
