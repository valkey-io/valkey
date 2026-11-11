# Test Raft log snapshot (InstallSnapshot) behavior.

source tests/support/cluster_raft.tcl

tags {external:skip cluster singledb} {

# The node IDs in each shard of CLUSTER SHARDS, sorted, for comparing the
# shard membership seen by different nodes.
proc shard_members {id} {
    set shards {}
    foreach shard [R $id cluster shards] {
        set ids {}
        foreach node [dict get $shard nodes] {
            lappend ids [dict get $node id]
        }
        lappend shards [lsort $ids]
    }
    return [lsort $shards]
}

start_cluster 3 0 {overrides {cluster-protocol raft cluster-node-timeout 3000}} {

    # Nodes join as learners. Make all three voters.
    raft_promote_start_cluster_voters 3
    test "Raft snapshot: follower catches up via snapshot after leader trims log" {
        assert_equal "leader" [CI 0 cluster_raft_role]

        # Stop a follower before generating entries so it falls behind the
        # leader's trim point. Stop it instead of pausing it, so it doesn't
        # catch up from entries buffered on its links when it comes back.
        catch {R 1 shutdown}

        # Generate enough committed entries to trigger log trimming (>128).
        for {set j 0} {$j < 140} {incr j} {
            R 0 config set cluster-announce-hostname "host-$j.com"
        }

        # Wait for leader to trim its log.
        wait_for_condition 50 100 {
            [CI 0 cluster_raft_log_entries] < 128
        } else {
            fail "leader did not trim log (entries=[CI 0 cluster_raft_log_entries])"
        }
        set leader_applied [CI 0 cluster_raft_last_applied]

        restart_server -1 true false

        # Follower should catch up via snapshot.
        wait_for_condition 50 200 {
            [CI 1 cluster_raft_last_applied] == [CI 0 cluster_raft_last_applied]
        } else {
            fail "follower did not catch up (follower=[CI 1 cluster_raft_last_applied] leader=[CI 0 cluster_raft_last_applied])"
        }

        # Verify the follower caught up to the leader's applied index.
        assert {[CI 1 cluster_raft_last_applied] >= $leader_applied}
        assert_morethan [count_log_message -1 "Installed snapshot"] 0
        assert_equal [shard_members 0] [shard_members 1]
    }

    test "Raft snapshot: new leader catches up a follower behind its trim point" {
        # A new leader starts at next_index = last + 1 for each follower and
        # steps back on rejection. For a follower behind the trim point, it
        # must step back past the trimmed boundary and send a snapshot,
        # rather than an AE with an unknown previous term.
        # R2 starts importing slot 0 from R0. The migration completes while
        # R2 is down, so R2 learns about it from the snapshot and must clear
        # its importing marker, which isn't part of the snapshot.
        set r0_id [R 0 cluster myid]
        set r2_id [R 2 cluster myid]
        assert_equal OK [R 2 cluster setslot 0 importing $r0_id]
        assert_equal OK [R 0 cluster setslot 0 migrating $r2_id]

        # Stop R2 instead of pausing it, so it doesn't receive the new
        # entries from data buffered on its links when it comes back.
        catch {R 2 shutdown}
        assert_equal OK [R 0 cluster setslot 0 node $r2_id]
        for {set j 0} {$j < 140} {incr j} {
            R 0 config set cluster-announce-hostname "host-b-$j.com"
        }
        wait_for_condition 50 100 {
            [CI 1 cluster_raft_last_applied] == [CI 0 cluster_raft_last_applied] &&
            [CI 1 cluster_raft_log_entries] < 128
        } else {
            fail "follower did not trim log (entries=[CI 1 cluster_raft_log_entries])"
        }

        # Make R1 the leader: stop R0 and restart R2, which votes for R1.
        pause_process [srv 0 pid]
        restart_server -2 true false
        wait_for_condition 100 200 {
            [CI 1 cluster_raft_role] eq "leader" &&
            [CI 2 cluster_raft_last_applied] == [CI 1 cluster_raft_last_applied]
        } else {
            fail "R2 did not catch up with the new leader (R2=[CI 2 cluster_raft_last_applied] R1=[CI 1 cluster_raft_last_applied] role=[CI 1 cluster_raft_role])"
        }
        assert_equal 1 [count_log_message -2 "Installed snapshot"]
        set myself_line [lsearch -inline [split [R 2 cluster nodes] "\n"] "*myself*"]
        assert_no_match {*\[0-<-*} $myself_line
        assert_equal 0 [lindex $myself_line 8]
        resume_process [srv 0 pid]
    }

    test "Raft snapshot: follower catches up after losing a snapshot" {
        # Pick the leader and a follower.
        wait_for_condition 100 100 {
            [lsearch [list [CI 0 cluster_raft_role] [CI 1 cluster_raft_role] [CI 2 cluster_raft_role]] leader] >= 0
        } else {
            fail "no leader"
        }
        set l [lsearch [list [CI 0 cluster_raft_role] [CI 1 cluster_raft_role] [CI 2 cluster_raft_role]] leader]
        set f [expr {($l + 1) % 3}]
        set f_id [R $f cluster myid]
        set installed_before [count_log_message -$f "Installed snapshot"]

        # The leader sends a snapshot to the paused follower, which only gets
        # it into its socket buffer.
        pause_process [srv -$f pid]
        set sent_before [count_log_message -$l "Sent INSTALL_SNAPSHOT to $f_id"]
        for {set j 0} {$j < 140} {incr j} {
            R $l config set cluster-announce-hostname "host-d-$j.com"
        }
        wait_for_condition 100 100 {
            [count_log_message -$l "Sent INSTALL_SNAPSHOT to $f_id"] > $sent_before
        } else {
            fail "leader did not send a snapshot (log entries=[CI $l cluster_raft_log_entries])"
        }

        # It isn't resent on every heartbeat, only after node-timeout/2.
        set sent [count_log_message -$l "Sent INSTALL_SNAPSHOT to $f_id"]
        after 1000
        assert_range [count_log_message -$l "Sent INSTALL_SNAPSHOT to $f_id"] $sent [expr {$sent + 1}]

        # Lose the snapshot by killing the follower before it reads it.
        exec kill -9 [srv -$f pid]
        restart_server -$f true false
        wait_for_condition 100 200 {
            [CI $f cluster_raft_last_applied] == [CI $l cluster_raft_last_applied]
        } else {
            fail "follower did not catch up (follower=[CI $f cluster_raft_last_applied] leader=[CI $l cluster_raft_last_applied])"
        }
        assert_equal [expr {$installed_before + 1}] [count_log_message -$f "Installed snapshot"]
        assert_morethan [count_log_message -$l "Sent INSTALL_SNAPSHOT to $f_id"] $sent
    }
}

start_cluster 4 1 {overrides {cluster-protocol raft cluster-node-timeout 3000}} {

    # Nodes join as learners. Make the primaries voters. The test takes R0
    # and its replica R4 down, which leaves a majority whether or not R4 is
    # a voter.
    raft_promote_start_cluster_voters 4
    test "Raft snapshot: replica fails over after learning from a snapshot that its primary failed" {
        # R4 is down while its primary R0 fails, so it skips the NODE_FAIL
        # entry and learns about the failure from a snapshot.
        set r0_id [R 0 cluster myid]
        assert_equal $r0_id [lindex [lsearch -inline [split [R 4 cluster nodes] "\n"] "*myself*"] 3]
        catch {R 4 shutdown}

        pause_process [srv 0 pid]
        wait_for_condition 100 100 {
            [string match "*fail*" [lindex [raft_cluster_nodes_line [srv -1 client] $r0_id] 2]]
        } else {
            fail "R0 was not marked as failed"
        }

        # Generate entries until R4 is behind the trim point of every voter.
        set applied [CI 1 cluster_raft_last_applied]
        for {set j 0} {$j < 140} {incr j} {
            R 1 config set cluster-announce-hostname "host-c-$j.com"
        }
        wait_for_condition 100 100 {
            [CI 1 cluster_raft_last_applied] >= $applied + 140 &&
            [CI 1 cluster_raft_log_entries] < 128 &&
            [CI 2 cluster_raft_log_entries] < 128 &&
            [CI 3 cluster_raft_log_entries] < 128
        } else {
            fail "log was not trimmed (entries=[CI 1 cluster_raft_log_entries])"
        }

        restart_server -4 true false
        wait_for_condition 100 200 {
            [s -4 role] eq "master"
        } else {
            fail "R4 did not fail over (role=[s -4 role])"
        }
        assert_equal 1 [count_log_message -4 "Installed snapshot"]
        assert_equal [shard_members 1] [shard_members 4]
        resume_process [srv 0 pid]
    }
}

start_cluster 3 0 {overrides {cluster-protocol raft cluster-node-timeout 3000}} {

    # Nodes join as learners. Make all three voters.
    raft_promote_start_cluster_voters 3
    test "Raft snapshot: new node joins leader with compacted log (via restart)" {
        assert_equal "leader" [CI 0 cluster_raft_role]
        set leader_port [srv 0 port]

        # Wait for cluster to converge.
        wait_for_condition 50 100 {
            [CI 2 cluster_raft_last_applied] == [CI 0 cluster_raft_last_applied]
        } else {
            fail "cluster not converged"
        }

        # Restart all nodes one at a time. This preserves quorum and avoids
        # a slow full election. After restart, each node's in-memory log is
        # empty (all committed entries are in the snapshot/nodes.conf).
        for {set i 0} {$i < 3} {incr i} {
            restart_server [expr {-1*$i}] true false
            # Wait for the restarted node to rejoin.
            wait_for_condition 50 100 {
                [CI $i cluster_raft_last_applied] > 0
            } else {
                fail "node $i did not recover after restart"
            }
        }

        # Add a new node. The leader has no log entries, so it must
        # send InstallSnapshot to bring the new node up to speed.
        start_server {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 3000}} {
            R 0 CLUSTER MEET 127.0.0.1 $leader_port

            # Wait for the new node to join (receives snapshot with all 4 nodes).
            wait_for_condition 200 200 {
                [CI 0 cluster_known_nodes] == 4
            } else {
                puts "New node CLUSTER NODES: [R 0 CLUSTER NODES]"
                puts "New node CLUSTER INFO: [R 0 CLUSTER INFO]"
                fail "new node did not join (known_nodes=[CI 0 cluster_known_nodes])"
            }

            # Verify snapshot was used and the new node's index is correct.
            assert {[count_log_message 0 "Installed snapshot"] > 0}
            assert {[CI 0 cluster_raft_last_applied] > 0}
        }
    }
}

start_cluster 3 0 {overrides {cluster-protocol raft cluster-node-timeout 3000}} {

    # Nodes join as learners. Make all three voters.
    raft_promote_start_cluster_voters 3
    test "Raft snapshot: new node joins when leader has trimmed log" {
        assert_equal "leader" [CI 0 cluster_raft_role]
        set leader_port [srv 0 port]

        # Generate enough entries to trigger trimming.
        for {set j 0} {$j < 140} {incr j} {
            R 0 config set cluster-announce-hostname "host-$j.com"
        }

        wait_for_condition 50 100 {
            [CI 0 cluster_raft_log_entries] < 128
        } else {
            fail "leader did not trim log"
        }

        # Start a new server and have it MEET the leader. The new node
        # steps down to joiner on WELCOME and receives InstallSnapshot.
        assert_equal "leader" [CI 0 cluster_raft_role]
        set leader_applied [CI 0 cluster_raft_last_applied]
        start_server {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 3000}} {
            # Here, R 0 is the new server and the others have shifted.
            R 0 CLUSTER MEET 127.0.0.1 $leader_port

            wait_for_condition 100 200 {
                [CI 0 cluster_known_nodes] == 4
            } else {
                fail "new node did not join (known_nodes=[CI 0 cluster_known_nodes])"
            }

            # Verify snapshot was used and the new node caught up. New nodes
            # join as learners.
            assert {[count_log_message 0 "Installed snapshot"] > 0}
            assert {[count_log_message 0 "Promoted from joiner to learner"] > 0}
            assert {[CI 0 cluster_raft_last_applied] >= $leader_applied}
            # The snapshot includes the slot assignments.
            assert_equal 16384 [CI 0 cluster_slots_assigned]
        }
    }
}

start_cluster 1 2 {overrides {cluster-protocol raft cluster-node-timeout 3000}} {

    test "Raft snapshot: singleton that has been in a cluster keeps its term and size on restart" {
        assert_equal "leader" [CI 0 cluster_raft_role]
        set r1_id [R 1 cluster myid]
        set r2_id [R 2 cluster myid]

        # Leave R0 alone. Demote the replicas first if they are voters, so
        # that R0 can commit the forgets on its own.
        foreach id [list $r1_id $r2_id] {
            catch {R 0 cluster delvoter $id}
        }
        wait_for_condition 50 100 {
            [CI 0 cluster_size] == 1
        } else {
            fail "replicas were not demoted (size=[CI 0 cluster_size])"
        }
        # Pause the replicas while they are forgotten, so that R0 still has
        # links to them, and check that the links are freed.
        pause_process [srv -1 pid]
        pause_process [srv -2 pid]
        assert_equal OK [R 0 cluster forget $r1_id]
        assert_equal OK [R 0 cluster forget $r2_id]
        assert_equal 1 [CI 0 cluster_known_nodes]
        assert_morethan [CI 0 cluster_raft_last_applied] 0
        assert_equal 0 [s 0 mem_cluster_links]
        resume_process [srv -1 pid]
        resume_process [srv -2 pid]
        catch {R 1 shutdown nosave}
        catch {R 2 shutdown nosave}
        set term [CI 0 cluster_raft_current_term]

        restart_server 0 true false
        wait_for_condition 100 100 {
            [CI 0 cluster_raft_role] eq "leader"
        } else {
            fail "R0 didn't become leader (role=[CI 0 cluster_raft_role])"
        }
        assert_morethan [CI 0 cluster_raft_current_term] $term
        assert_equal 1 [CI 0 cluster_size]

        # A node joining it as a voter is counted in the size.
        # A reset node has an empty log, so its last log term must be 0.
        # Otherwise it would claim a newer log than it has in elections.
        restart_server -1 true false
        R 1 cluster reset hard
        set nodes_conf [file join [lindex [R 1 config get dir] 1] [lindex [R 1 config get cluster-config-file] 1]]
        wait_for_condition 50 100 {
            [string match "*lastApplied 0 snapshotTerm 0*" [exec cat $nodes_conf]]
        } else {
            fail "snapshotTerm was not reset: [exec grep vars $nodes_conf]"
        }
        set r1_id [R 1 cluster myid]
        R 0 cluster meet 127.0.0.1 [srv -1 port]
        raft_add_voter [srv 0 client] $r1_id
        wait_for_condition 50 100 {
            [CI 0 cluster_size] == 2
        } else {
            fail "joined voter not counted (size=[CI 0 cluster_size])"
        }
    }
}

} ;# tags
