start_cluster 3 4 {tags {external:skip cluster} overrides {cluster-ping-interval 1000 cluster-node-timeout 5000 cluster-replica-validity-factor 0}} {
    test "Replica can do a better ranking in auto failover based on the priority" {
        # primary R 0, replica1 R 3, replica2 R 6

        # Write a key to primary 0, slot 1, make a small repl_offset.
        set small_value [string repeat "x" 1024]
        R 0 set key_991803 [string repeat "x" 1024]
        wait_for_ofs_sync [srv 0 client] [srv -3 client]
        wait_for_ofs_sync [srv 0 client] [srv -6 client]

        # Set two different priorities, R 3 will have a better priority.
        R 3 config set cluster-replica-priority 0
        R 6 config set cluster-replica-priority 10
        wait_for_condition 1000 50 {
            [R 3 debug cluster-replica-priority [R 6 cluster myid]] == 10 &&
            [R 6 debug cluster-replica-priority [R 3 cluster myid]] == 0
        } else {
            fail "Replica priority did not propagate"
        }

        # R 3 has a better priority and will become the primary.
        pause_process [srv 0 pid]
        wait_for_condition 1000 50 {
            [s -3 role] eq {master} &&
            [s -6 role] eq {slave}
        } else {
            fail "The first failover did not go as expected"
        }

        # Make sure our rank and priority are correct.
        verify_log_message -3 "*Start of election*rank #0*replica priority 0*" 0
        verify_log_message -6 "*Start of election*rank #1*replica priority 10*" 0

        resume_process [srv 0 pid]
        wait_for_condition 1000 50 {
            [s 0 role] eq {slave} &&
            [s -3 role] eq {master} &&
            [s -6 role] eq {slave}
        } else {
            fail "The old primary was not converted into replica"
        }

        # Set two different priorities, R 6 will have a better priority.
        # We also modified the priority of R 6 to verify that the propagation was normal.
        R 0 config set cluster-replica-priority 10
        R 6 config set cluster-replica-priority 5
        wait_for_condition 1000 50 {
            [R 6 debug cluster-replica-priority [R 0 cluster myid]] == 10 &&
            [R 0 debug cluster-replica-priority [R 6 cluster myid]] == 5
        } else {
            fail "Replica priority did not propagate"
        }

        # R 6 has a better priority and will become the primary.
        pause_process [srv -3 pid]
        wait_for_condition 1000 50 {
            [s 0 role] eq {slave} &&
            [s -6 role] eq {master}
        } else {
            fail "The second failover did not go as expected"
        }

        # Make sure our rank and priority are correct.
        verify_log_message 0 "*Start of election*rank #1*replica priority 10*" 0
        verify_log_message -6 "*Start of election*rank #0*replica priority 5*" 0

        resume_process [srv -3 pid]
        wait_for_condition 1000 50 {
            [s 0 role] eq {slave} &&
            [s -3 role] eq {slave} &&
            [s -6 role] eq {master}
        } else {
            fail "The old primary was not converted into replica"
        }

        # Replica priority resets to 0 when the sender stops advertising it
        R 0 config set cluster-replica-priority 0
        wait_for_condition 1000 50 {
            [R 3 debug cluster-replica-priority [R 0 cluster myid]] == 0 &&
            [R 6 debug cluster-replica-priority [R 0 cluster myid]] == 0
        } else {
            fail "Replica priority did not propagate"
        }
    }

    test "Non-zero replica priority is persisted in nodes.conf" {
        set CLUSTER_PACKET_TYPE_NONE -1
        set CLUSTER_PACKET_TYPE_ALL -2

        set R0_nodeid [R 0 cluster myid]
        set R6_nodeid [R 6 cluster myid]

        R 0 config set cluster-replica-priority 10
        R 3 config set cluster-replica-priority 20
        R 6 config set cluster-replica-priority 30

        set nodes_conf0 [cluster_nodes_conf_path 0]
        set nodes_conf3 [cluster_nodes_conf_path 3]
        set nodes_conf6 [cluster_nodes_conf_path 6]

        # Each node should learn its peers' non-zero priorities and write
        # them to its own nodes.conf via the replica-priority aux field.
        wait_for_condition 1000 50 {
            [file_has_pattern $nodes_conf0 {replica-priority=10}] &&
            [file_has_pattern $nodes_conf0 {replica-priority=20}] &&
            [file_has_pattern $nodes_conf0 {replica-priority=30}] &&
            [file_has_pattern $nodes_conf3 {replica-priority=10}] &&
            [file_has_pattern $nodes_conf3 {replica-priority=20}] &&
            [file_has_pattern $nodes_conf3 {replica-priority=30}] &&
            [file_has_pattern $nodes_conf6 {replica-priority=10}] &&
            [file_has_pattern $nodes_conf6 {replica-priority=20}] &&
            [file_has_pattern $nodes_conf6 {replica-priority=30}]
        } else {
            fail "Non-zero replica priority was not persisted to nodes.conf"
        }

        # A node whose priority is 0 (the default, highest failover rank) must not
        # be written to nodes.conf.
        R 0 config set cluster-replica-priority 0
        wait_for_condition 1000 50 {
            ![file_has_pattern $nodes_conf0 {replica-priority=0}] &&
            ![file_has_pattern $nodes_conf0 {replica-priority=10}] &&
            ![file_has_pattern $nodes_conf3 {replica-priority=0}] &&
            ![file_has_pattern $nodes_conf3 {replica-priority=10}] &&
            ![file_has_pattern $nodes_conf6 {replica-priority=0}] &&
            ![file_has_pattern $nodes_conf6 {replica-priority=10}]
        } else {
            fail "Zero replica priority was unexpectedly persisted to nodes.conf"
        }

        # Restart node 3 and make sure it restores the learned peer priorities
        # from nodes.conf.
        R 3 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_ALL
        R 0 config set cluster-replica-priority 100
        R 6 config set cluster-replica-priority 300
        pause_process [srv 0 pid]
        pause_process [srv -6 pid]
        restart_server -3 true false
        assert_equal [R 3 debug cluster-replica-priority $R0_nodeid] 0
        assert_equal [R 3 debug cluster-replica-priority $R6_nodeid] 30

        # Everything will be alright.
        R 3 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_NONE
        resume_process [srv 0 pid]
        resume_process [srv -6 pid]
        wait_for_condition 1000 50 {
            [R 3 debug cluster-replica-priority $R0_nodeid] == 100 &&
            [R 3 debug cluster-replica-priority $R6_nodeid] == 300
        } else {
            fail "Replica priority did not propagate"
        }
    }
} ;# start_cluster
