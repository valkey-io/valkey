# Manual takeover test

start_cluster 5 5 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test [srv -1 port]
}

# For this test, disable replica failover until
# all of the primaries are confirmed killed. Otherwise
# there might be enough time to elect a replica.
set replica_ids { 5 6 7 }
foreach id $replica_ids {
    R $id config set cluster-replica-no-failover yes
}

set paused_pid [srv 0 pid]
set paused_pid1 [srv -1 pid]
set paused_pid2 [srv -2 pid]
test "Killing majority of master nodes" {
    pause_process_persistent $paused_pid
    pause_process_persistent $paused_pid1
    pause_process_persistent $paused_pid2
}

test "Cluster should eventually be down" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {[process_is_paused [srv -$j pid]]} continue
        wait_for_condition 1000 50 {
            [CI $j cluster_state] eq "fail"
        } else {
            fail "Cluster node $j cluster_state:[CI $j cluster_state]"
        }
    }
}

test "Use takeover to bring slaves back" {
    foreach id $replica_ids {
        R $id cluster failover takeover
    }
}

test "Cluster should eventually be up again" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {[process_is_paused [srv -$j pid]]} continue
        wait_for_condition 1000 50 {
            [CI $j cluster_state] eq "ok"
        } else {
            fail "Cluster node $j cluster_state:[CI $j cluster_state]"
        }
    }
}

test "Cluster is writable" {
    cluster_write_test [srv -4 port]
}

test "Instance #5, #6, #7 are now masters" {
    assert {[s -5 role] eq {master}}
    assert {[s -6 role] eq {master}}
    assert {[s -7 role] eq {master}}
}

test "Restarting the previously killed master nodes" {
    resume_process $paused_pid
    resume_process $paused_pid1
    resume_process $paused_pid2
}

test "Instance #0, #1, #2 gets converted into a slaves" {
    wait_for_condition 1000 50 {
        [s 0 role] eq {slave} && [s -1 role] eq {slave} && [s -2 role] eq {slave}
    } else {
        fail "Old masters not converted into slaves"
    }
}

} ;# start_cluster

start_cluster 1 2 {tags {external:skip cluster} overrides {repl-diskless-load disabled}} {
    test "Replica resyncs when its primary changes while it loads a full sync" {
        # DEBUG POPULATE does not propagate, so give replica 2 the same keys.
        R 0 debug populate 100000 key 100
        R 2 debug populate 100000 key 100
        # Slow the load on replica 1 so the takeover lands mid-load, then
        # force it into a full sync.
        R 1 config set key-load-delay 50
        R 1 config set loading-process-events-interval-bytes 1024
        R 0 debug change-repl-id
        R 1 client kill type primary
        wait_for_condition 100 50 {
            [s -1 loading] eq 1
        } else {
            fail "Replica did not start loading"
        }

        # A write the snapshot replica 1 is loading does not have.
        R 0 set foo bar
        wait_for_ofs_sync [srv 0 client] [srv -2 client]

        # The new primary's slot claim reaches replica 1 over the cluster bus
        # while it is still loading.
        R 2 cluster failover takeover
        wait_for_condition 1000 50 {
            [s -1 loading] eq 0
        } else {
            fail "Replica did not finish loading"
        }
        R 1 config set key-load-delay 0

        wait_for_condition 1000 50 {
            [catch {R 1 dbsize} size] == 0 && $size == 100001
        } else {
            fail "Replica kept the old primary's dataset"
        }
        # The takeover must have landed mid-load for this test to mean anything.
        verify_log_message -1 "*Sync was cancelled while loading the DB*" 0
    }
}
