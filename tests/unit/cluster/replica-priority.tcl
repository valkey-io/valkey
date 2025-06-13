start_cluster 3 4 {tags {external:skip cluster} overrides {cluster-ping-interval 1000 cluster-node-timeout 2000}} {
    test "Replica can do a better ranking in auto failover based on the priority" {
        # primary R 0, replica1 R 3, replica2 R 6

        # Write some data to primary 0, slot 1, make a small repl_offset.
        for {set i 0} {$i < 1024} {incr i} {
            R 0 incr key_991803
        }
        wait_for_ofs_sync [srv 0 client] [srv -3 client]
        wait_for_ofs_sync [srv 0 client] [srv -6 client]

        # Set two different priorities, R 3 will have a better priority.
        R 3 config set cluster-replica-priority 0
        R 6 config set cluster-replica-priority 10

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

        # Set two different priorities, R 0 will have a better priority.
        # We also modified the priority of R 6 to verify that the propagation was normal.
        R 0 config set cluster-replica-priority 10
        R 6 config set cluster-replica-priority 0

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
        verify_log_message -6 "*Start of election*rank #0*replica priority 0*" 0

        resume_process [srv -3 pid]
        wait_for_condition 1000 50 {
            [s 0 role] eq {slave} &&
            [s -3 role] eq {slave} &&
            [s -6 role] eq {master}
        } else {
            fail "The old primary was not converted into replica"
        }
    }
} ;# start_cluster
