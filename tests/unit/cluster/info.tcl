# Check cluster info stats

start_cluster 2 0 {tags {external:skip cluster}} {

test "Cluster should start ok" {
    wait_for_cluster_state ok
}

set primary1 [srv 0 "client"]
set primary2 [srv -1 "client"]

proc cmdstat {instance cmd} {
    return [cmdrstat $cmd $instance]
}

proc errorstat {instance cmd} {
    return [errorrstat $cmd $instance]
}

test "errorstats: rejected call due to MOVED Redirection" {
    $primary1 config resetstat
    $primary2 config resetstat
    assert_match {} [errorstat $primary1 MOVED]
    assert_match {} [errorstat $primary2 MOVED]
    # we know that one will have a MOVED reply and one will succeed
    catch {$primary1 set key b} replyP1
    catch {$primary2 set key b} replyP2
    # sort servers so we know which one failed
    if {$replyP1 eq {OK}} {
        assert_match {MOVED*} $replyP2
        set pok $primary1
        set perr $primary2
    } else {
        assert_match {MOVED*} $replyP1
        set pok $primary2
        set perr $primary1
    }
    assert_match {} [errorstat $pok MOVED]
    assert_match {*count=1*} [errorstat $perr MOVED]
    assert_match {*calls=0,*,rejected_calls=1,failed_calls=0} [cmdstat $perr set]
}

} ;# start_cluster

start_cluster 3 0 {tags {external:skip cluster} overrides {cluster-node-timeout 1000}} {
    test "fail reason changed" {
        # Kill one primary, so the cluster fail with not-full-coverage.
        pause_process [srv 0 pid]
        wait_for_condition 1000 50 {
            [CI 1 cluster_state] eq {fail} &&
            [CI 2 cluster_state] eq {fail}
        } else {
            fail "Cluster doesn't fail"
        }
        verify_log_message -1 "*At least one hash slot is not served by any available node*" 0
        verify_log_message -2 "*At least one hash slot is not served by any available node*" 0

        # Kill one more primary, so the cluster fail with minority-partition.
        pause_process [srv -1 pid]
        wait_for_log_messages -2 {"*minority partition*"} 0 1000 50

        resume_process [srv 0 pid]
        resume_process [srv -1 pid]
        wait_for_cluster_state ok
    }
}

start_cluster 3 0 {tags {external:skip cluster} overrides {cluster-node-timeout 1000}} {
    # Kill two primaries to observe partial failure on the remaining one.
    pause_process [srv 0 pid]
    pause_process [srv -1 pid]

    test "count - node partial failure" {
        wait_for_condition 500 10 {
            [CI 2 cluster_nodes_pfail] eq 2 &&
            [CI 2 cluster_nodes_fail] eq 0 &&
            [CI 2 cluster_voting_nodes_pfail] eq 2 &&
            [CI 2 cluster_voting_nodes_fail] eq 0
        } else {
            puts [R 2 CLUSTER INFO]
            fail "Node 0/1 never timed out"
        }
    }

    # Enable one more primary to reach quorum about node 0 failure
    resume_process [srv -1 pid]

    test "count - node complete failure" {
        # After reaching quorum about failure,
        # node 0 should be marked as FAIL across all nodes in the cluster
        wait_for_condition 100 100 {
            [CI 1 cluster_nodes_fail] eq 1 &&
            [CI 2 cluster_nodes_fail] eq 1 &&
            [CI 1 cluster_nodes_pfail] eq 0 &&
            [CI 2 cluster_nodes_pfail] eq 0 &&
            [CI 1 cluster_voting_nodes_fail] eq 1 &&
            [CI 2 cluster_voting_nodes_fail] eq 1

        } else {
            fail "Node 0 never completely failed"
        }
    }
}
