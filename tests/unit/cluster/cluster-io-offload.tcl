# Cluster I/O offload integration coverage.

start_cluster 3 0 {tags {external:skip cluster} overrides {io-threads 4 io-threads-always-active yes}} {

    test "cluster boots and reaches ok with io-threads > 1" {
        assert {[lindex [R 0 config get io-threads] 1] > 1}
        wait_for_cluster_state ok
    }

    test "reconnect churn with node restart reforms cluster" {
        for {set i 0} {$i < 3} {incr i} {
            restart_server -1 true false
            wait_for_cluster_state ok
        }
    }

    test "buffer-limit enforcement increments stat with small cluster-link-sendbuf-limit" {
        set oldlimit [lindex [R 0 config get cluster-link-sendbuf-limit] 1]
        set old_exceeded [CI 0 total_cluster_links_buffer_limit_exceeded]
        R 0 config set cluster-link-sendbuf-limit 1

        # Cluster bus gossip is already active; with a 1-byte limit, the next
        # outbound cluster message should force the offending link closed.
        try {
            wait_for_condition 500 10 {
                [CI 0 total_cluster_links_buffer_limit_exceeded] > $old_exceeded
            } else {
                fail "cluster links buffer limit stat did not increase"
            }
        } finally {
            # Restore limit and require that the cluster converges back to ok.
            R 0 config set cluster-link-sendbuf-limit $oldlimit
            wait_for_cluster_state ok
        }
    }

    test "disabling io threads on one node falls back to main-thread I/O" {
        set old_threads [lindex [R 0 config get io-threads] 1]
        R 0 config set io-threads 1
        R 0 config resetstat

        try {
            wait_for_condition 500 10 {
                [CI 0 cluster_stats_messages_sent] > 0 &&
                [CI 0 cluster_stats_messages_received] > 0 &&
                [CI 0 cluster_io_main_thread_fallbacks] > 0
            } else {
                fail "cluster io main-thread fallback stat did not increase after disabling io threads"
            }
        } finally {
            R 0 config set io-threads $old_threads
            wait_for_cluster_state ok
        }
    }
} ;# start_cluster


# TLS cluster offload retry/stability coverage.

if {$::tls} {
    start_cluster 3 3 {tags {external:skip cluster tls} overrides {tls-cluster yes tls-replication yes io-threads 4 io-threads-always-active yes}} {

        test "tls cluster with io-threads > 1 reaches ok" {
            assert {[lindex [R 0 config get io-threads] 1] > 1}
            wait_for_cluster_state ok
        }

        test "restart one node repeatedly and cluster reforms" {
            for {set i 0} {$i < 3} {incr i} {
                restart_server -1 true false
                wait_for_cluster_state ok
            }
        }

        test "cluster_io_threaded_accepts_processed increments under tls reconnect churn" {
            wait_for_condition 500 10 {
                [CI 0 cluster_io_threaded_accepts_processed] > 0 || \
                [CI 1 cluster_io_threaded_accepts_processed] > 0 || \
                [CI 2 cluster_io_threaded_accepts_processed] > 0
            } else {
                fail "cluster_io_threaded_accepts_processed did not increase"
            }
        }

        test "no stuck handshake and no crash after churn" {
            wait_for_cluster_state ok
        }
    }
}
