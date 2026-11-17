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

start_cluster 3 3 {tags {external:skip cluster}} {
    set key "{06S}key"

    test "Paused primary does not acknowledge writes without keys after its replica took over" {
        assert_equal {OK} [R 0 set $key before-failover]
        assert_equal {1} [R 0 wait 1 5000]

        # Commands without keys skip the slot check, so the stall check has to
        # refuse them by their write flag. The last client queues a FLUSHALL in
        # a transaction before the pause and sends the EXEC during it.
        set cmds {flushall flushdb {function flush} exec}
        set clients {}
        foreach cmd $cmds {
            lappend clients [valkey_deferring_client 0]
        }
        set rd_exec [lindex $clients end]
        $rd_exec multi
        assert_equal {OK} [$rd_exec read]
        $rd_exec flushall
        assert_equal {QUEUED} [$rd_exec read]

        set primary_pid [srv 0 pid]
        pause_process $primary_pid
        foreach rd $clients cmd $cmds {
            $rd {*}$cmd
        }

        wait_for_condition 1000 50 {
            [s -3 role] eq {master}
        } else {
            fail "Replica 3 was not promoted"
        }
        resume_process $primary_pid

        foreach rd $clients cmd $cmds {
            catch {$rd read} reply
            if {![regexp {^(CLUSTERDOWN|MOVED) } $reply]} {
                fail "Paused primary replied '$reply' to [string toupper $cmd] after failover"
            }
            $rd close
        }

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

    test "Paused primary does not acknowledge a write without keys inside a script after its replica took over" {
        # Mixing a keyed write with a keyless one is fine while the cluster is
        # healthy.
        assert_equal {OK} [R 0 eval "server.call('set', KEYS\[1\], 'v') return server.call('flushall')" 1 $key]
        assert_equal {OK} [R 0 set $key before-failover]
        assert_equal {1} [R 0 wait 1 5000]

        # An EVAL without a shebang keeps EVAL's own flags, so it is not a
        # write when admitted. The FLUSHALL it calls has no keys, so only a
        # stall check in the script's own call path can refuse it.
        set rd [valkey_deferring_client 0]
        set primary_pid [srv 0 pid]
        pause_process $primary_pid
        $rd eval "return server.call('flushall')" 0

        wait_for_condition 1000 50 {
            [s -3 role] eq {master}
        } else {
            fail "Replica 3 was not promoted"
        }
        resume_process $primary_pid

        catch {$rd read} reply
        if {![regexp {^(CLUSTERDOWN|MOVED) |while the cluster is down} $reply]} {
            fail "Paused primary replied '$reply' to a script FLUSHALL after failover"
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

    test "Primary does not acknowledge a write after a batch of commands that outlasts the node timeout" {
        assert_equal {OK} [R 0 set $key before-failover]
        assert_equal {1} [R 0 wait 1 5000]

        # Each DEBUG SLEEP is shorter than the node timeout, and all of them run
        # in one event batch. Only the time since the last poll shows the stall,
        # as when the process is stopped between two commands of a batch.
        set rd [valkey_deferring_client 0]
        for {set j 0} {$j < 5} {incr j} {
            $rd write [format_command debug sleep 2]
        }
        $rd write [format_command set $key after-failover]
        $rd flush

        wait_for_condition 1000 50 {
            [s -3 role] eq {master}
        } else {
            fail "Replica 3 was not promoted"
        }

        for {set j 0} {$j < 5} {incr j} {
            assert_equal {OK} [$rd read]
        }
        catch {$rd read} reply
        if {![regexp {^(CLUSTERDOWN|MOVED) } $reply]} {
            fail "Primary replied '$reply' to a write after a long batch"
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

# Stops primary 0 while it runs writes pipelined by many clients, and checks that
# no write that started after it resumed was acknowledged. Each script takes
# about a millisecond and returns the time it started at. The pipelines hold
# several seconds of work, so the stop lands in the middle of a batch.
proc test_primary_stopped_under_write_load {} {
    set clients 16
    set writes 300
    set script {local t = redis.call('time') for i = 1, 1000000 do end redis.call('set', KEYS[1], 'v') return t}
    set cmd [format_command eval $script 1 "{06S}load"]

    # Connect all clients first. A new connection waits for the work queued
    # before it.
    set rds {}
    for {set j 0} {$j < $clients} {incr j} {
        lappend rds [valkey_deferring_client 0]
    }
    foreach rd $rds {
        $rd write [string repeat $cmd $writes]
        $rd flush
    }
    after 500

    set primary_pid [srv 0 pid]
    pause_process $primary_pid
    wait_for_condition 1000 50 {
        [s -3 role] eq {master}
    } else {
        fail "Replica 3 was not promoted"
    }
    set resumed_us [clock microseconds]
    resume_process $primary_pid

    set acked_after_resume 0
    set refused 0
    foreach rd $rds {
        for {set j 0} {$j < $writes} {incr j} {
            if {[catch {$rd read} reply]} {
                if {![regexp {^(CLUSTERDOWN|MOVED) } $reply]} {
                    fail "Unexpected reply '$reply'"
                }
                incr refused
            } else {
                lassign $reply sec usec
                if {$sec * 1000000 + $usec >= $resumed_us} {
                    incr acked_after_resume
                }
            }
        }
        $rd close
    }
    assert_equal 0 $acked_after_resume
    assert_morethan $refused 0

    wait_for_condition 1000 50 {
        [s 0 role] eq {slave}
    } else {
        fail "Old primary did not become a replica"
    }
}

start_cluster 3 3 {tags {external:skip cluster}} {
    test "Primary stopped under write load does not acknowledge writes after failover" {
        test_primary_stopped_under_write_load
    }
}

start_cluster 3 3 {tags {external:skip cluster} overrides {io-threads 4 io-threads-always-active yes}} {
    test "Primary stopped under write load with I/O threads does not acknowledge writes after failover" {
        test_primary_stopped_under_write_load
    }
}

start_cluster 3 3 {tags {external:skip cluster}} {
    set key "{06S}key"

    test "Primary does not acknowledge a write that arrives during a slow AOF write" {
        R 0 config set appendonly yes
        wait_for_condition 1000 50 {
            [s 0 aof_rewrite_in_progress] == 0 && [s 0 aof_rewrite_scheduled] == 0
        } else {
            fail "AOF rewrite did not finish"
        }
        assert_equal {OK} [R 0 set $key before-failover]
        assert_equal {1} [R 0 wait 1 5000]

        # The next AOF write blocks the main thread for longer than the node
        # timeout. The reply to the write that fills the AOF buffer is sent
        # after the block, which is a known limit, so it is not checked.
        R 0 debug aof-flush-sleep 8000000
        set rd1 [valkey_deferring_client 0]
        set rd2 [valkey_deferring_client 0]
        $rd1 set "{06S}other" value

        wait_for_condition 1000 50 {
            [s -3 role] eq {master}
        } else {
            fail "Replica 3 was not promoted"
        }
        $rd2 set $key after-failover
        catch {$rd2 read} reply
        if {![regexp {^(CLUSTERDOWN|MOVED) } $reply]} {
            fail "Primary replied '$reply' to a write that arrived during a slow AOF write"
        }
        catch {$rd1 read}
        $rd1 close
        $rd2 close
        R 0 debug aof-flush-sleep 0

        wait_for_condition 1000 50 {
            [s 0 role] eq {slave}
        } else {
            fail "Old primary did not become a replica"
        }
        assert_equal {before-failover} [R 3 get $key]
    }
}
