# A primary that was blocked for longer than cluster-node-timeout may already
# have been failed over. It must not acknowledge writes on wake-up until it has
# heard from a majority of primaries again, or the writes are lost when it is
# demoted.

start_cluster 3 3 {tags {external:skip cluster}} {
    # {06S} hashes to slot 0, which belongs to primary 0.
    set key "{06S}key"

    test "Paused primary does not acknowledge writes after its replica took over" {
        assert_equal {OK} [R 0 set $key before-failover]
        assert_equal {1} [R 0 wait 1 5000]

        set rd [valkey_deferring_client 0]
        set primary_pid [srv 0 pid]
        pause_process $primary_pid

        # Queued in the socket while the primary is paused, so the primary reads
        # it as soon as it resumes.
        $rd set $key after-failover

        wait_for_condition 1000 50 {
            [s -3 role] eq {master}
        } else {
            fail "Replica 3 was not promoted"
        }
        resume_process $primary_pid

        catch {$rd read} reply
        if {![regexp {^(CLUSTERDOWN|MOVED) } $reply]} {
            fail "Paused primary replied '$reply' to a write after failover"
        }
        $rd close

        wait_for_condition 1000 50 {
            [s 0 role] eq {slave}
        } else {
            fail "Old primary did not become a replica"
        }
        assert_equal {before-failover} [R 3 get $key]
    }
}

start_cluster 3 3 {tags {external:skip cluster}} {
    set key "{06S}key"

    test "Primary serves writes again after a long pause without failover" {
        R 3 config set cluster-replica-no-failover yes

        set primary_pid [srv 0 pid]
        pause_process $primary_pid
        after 4000
        resume_process $primary_pid

        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {ok}
        } else {
            fail "Primary 0 did not recover cluster_state ok"
        }
        verify_log_message 0 "*longer than cluster-node-timeout*" 0
        assert_equal {OK} [R 0 set $key after-pause]
        assert_equal {master} [s 0 role]
    }
}

start_cluster 3 3 {tags {external:skip cluster}} {
    set key "{06S}key"

    test "Primary does not acknowledge a write pipelined behind a slow command" {
        assert_equal {OK} [R 0 set $key before-failover]
        assert_equal {1} [R 0 wait 1 5000]

        # The DEBUG SLEEP outlasts the node timeout and the failover, and the
        # SET behind it runs in the same event batch, with no event loop sleep
        # between them.
        set rd [valkey_deferring_client 0]
        $rd write [format_command debug sleep 10]
        $rd write [format_command set $key after-failover]
        $rd flush

        wait_for_condition 1000 50 {
            [s -3 role] eq {master}
        } else {
            fail "Replica 3 was not promoted"
        }

        assert_equal {OK} [$rd read]
        catch {$rd read} reply
        if {![regexp {^(CLUSTERDOWN|MOVED) } $reply]} {
            fail "Primary replied '$reply' to a pipelined write after failover"
        }
        $rd close

        wait_for_condition 1000 50 {
            [s 0 role] eq {slave}
        } else {
            fail "Old primary did not become a replica"
        }
        assert_equal {before-failover} [R 3 get $key]
    }
}

start_cluster 3 3 {tags {external:skip cluster}} {
    set key "{06S}key"

    test "Replica blocked while its primary fails can still win the election" {
        assert_equal {OK} [R 0 set $key value]
        assert_equal {1} [R 0 wait 1 5000]

        # Both outlast the node timeout, so the replica wakes with the FAIL
        # message about its primary queued and starts its election at once.
        set primary_pid [srv 0 pid]
        pause_process $primary_pid
        set rd [valkey_deferring_client -3]
        $rd debug sleep 6
        assert_equal {OK} [$rd read]
        $rd close

        wait_for_condition 100 50 {
            [s -3 role] eq {master}
        } else {
            fail "Replica 3 was not promoted"
        }
        verify_no_log_message -3 "*Failover attempt expired*" 0
        resume_process $primary_pid
    }
}
