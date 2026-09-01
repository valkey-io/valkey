start_cluster 1 1 {tags {external:skip cluster}} {
    test "Cluster is up" {
        wait_for_cluster_state ok
    }

    test {blocked clients behavior during failover} {
        # A client blocking on the primary
        set rd0 [valkey_deferring_client 0]
        $rd0 BLPOP mylist 0

        # A READONLY client blocking on the primary
        set rd0_ro [valkey_deferring_client 0]
        $rd0_ro READONLY
        assert_equal OK [$rd0_ro read]
        $rd0_ro XREAD BLOCK 0 STREAMS mystream 0-0

        # A READONLY client blocking on the replica
        set rd1 [valkey_deferring_client -1]
        $rd1 READONLY
        assert_equal OK [$rd1 read]
        $rd1 XREAD BLOCK 0 STREAMS k 0-0

        wait_for_condition 1000 50 {
            [s 0 blocked_clients] eq 2 &&
            [s -1 blocked_clients] eq 1
        } else {
            fail "client wasn't blocked"
        }

        R 1 CLUSTER FAILOVER

        wait_for_condition 1000 50 {
            [s -1 role] eq {master} &&
            [s 0 role] eq {slave}
        } else {
            fail "The failover does not happen"
        }

        # Check that the client blocking on the old primary was MOVED to the new primary.
        assert_error "MOVED *" {$rd0 read}

        # Check that the readonly client blocking on the old primary is still blocked.
        assert_equal 1 [s 0 blocked_clients]

        # Check that the client blocked on the new primary (old replica) is still blocked.
        assert_equal 1 [s -1 blocked_clients]

        # Add an entry to the stream to unblock the blocking XREAD.
        set stream_id [r -1 XADD k * foo bar]
        assert_equal "{k {{$stream_id {foo bar}}}}" [$rd1 read]

        set stream_id [r -1 XADD mystream * foo bar]
        assert_equal "{mystream {{$stream_id {foo bar}}}}" [$rd0_ro read]

        $rd0 close
        $rd0_ro close
        $rd1 close
    }

    # Determine which node is the replica after failover
    set ridx [expr {[s 0 role] eq {slave} ? 0 : -1}]

    test {keyless commands execute on replica without redirect capa} {
        # Without the capa, keyless commands like SCAN execute locally on the replica
        set rd [valkey_deferring_client $ridx]
        $rd SCAN 0
        set reply [$rd read]
        assert_no_match "*REDIRECT*" $reply

        # Write keyless commands rejected
        $rd FLUSHDB
        assert_error "READONLY You can't write against a read only replica." {$rd read}
        
        $rd close
    }

    test {keyless commands are redirected with redirect capa} {
        set rd [valkey_deferring_client $ridx]
        $rd CLIENT CAPA redirect
        assert_equal OK [$rd read]
        # Read keyless commands redirected
        $rd DBSIZE
        assert_error "REDIRECT *" {$rd read}

        $rd RANDOMKEY
        assert_error "REDIRECT *" {$rd read}

        $rd SCAN 0
        assert_error "REDIRECT *" {$rd read}

        # Write keyless commands also redirected
        $rd FLUSHDB
        assert_error "REDIRECT *" {$rd read}

        $rd close
    }

    test {keyless commands execute on replica with redirect capa and READONLY} {
        set rd [valkey_deferring_client $ridx]
        $rd CLIENT CAPA redirect
        assert_equal OK [$rd read]
        $rd READONLY
        assert_equal OK [$rd read]

        # With READONLY, keyless commands should execute locally
        $rd DBSIZE
        set reply [$rd read]
        assert {$reply >= 0}

        $rd close
    }

    test {non-read & non-write keyless commands are not affected by redirect capa} {
        set rd [valkey_deferring_client $ridx]
        $rd CLIENT CAPA redirect
        assert_equal OK [$rd read]

        # PING should still work on replica
        $rd PING
        assert_equal PONG [$rd read]

        $rd close
    }

    test {CLIENT INFO reports redirect capa} {
        set rd [valkey_deferring_client $ridx]
        $rd CLIENT CAPA redirect
        assert_equal OK [$rd read]

        $rd CLIENT INFO
        assert_match "*capa=r*" [$rd read]

        $rd close
    }

    test {keyless commands inside MULTI are individually redirected with redirect capa} {
        set rd [valkey_deferring_client $ridx]
        $rd CLIENT CAPA redirect
        assert_equal OK [$rd read]

        $rd MULTI
        assert_equal OK [$rd read]
       # Individual keyless commands get REDIRECT (similar to how keyed commands get MOVED)
        $rd DBSIZE
        assert_error "REDIRECT *" {$rd read}
        $rd RANDOMKEY
        assert_error "REDIRECT *" {$rd read}
        # Write keyless commands also redirected
        $rd FLUSHDB
        assert_error "REDIRECT *" {$rd read}
        # Transaction was flagged dirty, EXEC returns EXECABORT
        $rd EXEC
        assert_error "EXECABORT *" {$rd read}

        $rd PING
        assert_equal PONG [$rd read]

        $rd close
    }

    test {EXEC with all-keyless commands is redirected after failover with redirect capa} {
        # Determine current roles (using server indices 0 and 1 for R command)
        set pidx [expr {[s 0 role] eq {master} ? 0 : -1}]
        # R command uses absolute server numbers
        set replica_srv [expr {$pidx == 0 ? 1 : 0}]

        # Connect to the primary with redirect capa
        set rd [valkey_deferring_client $pidx]
        $rd CLIENT CAPA redirect
        assert_equal OK [$rd read]

        # Queue keyless commands on the primary — they get QUEUED
        # because we're on a primary (redirect only fires on replicas)
        $rd MULTI
        assert_equal OK [$rd read]
        $rd DBSIZE
        assert_equal QUEUED [$rd read]
        $rd RANDOMKEY
        assert_equal QUEUED [$rd read]

        # Failover: the replica takes over, primary becomes a replica
        R $replica_srv CLUSTER FAILOVER
        wait_for_condition 1000 50 {
            [s $pidx role] eq {slave}
        } else {
            fail "Failover did not happen"
        }

        # EXEC is now on a replica with all-keyless queued commands.
        # The transaction should be discarded and REDIRECT returned.
        $rd EXEC
        assert_error "REDIRECT *" {$rd read}

        # Client should be usable after the discarded transaction
        $rd PING
        assert_equal PONG [$rd read]

        $rd close

        # Update ridx since roles changed
        set ridx [expr {[s 0 role] eq {slave} ? 0 : -1}]
    }

    test {redirect capa handles both keyed and keyless redirects} {
        set rd [valkey_deferring_client $ridx]
        $rd CLIENT CAPA redirect
        assert_equal OK [$rd read]

        $rd CLIENT INFO
        assert_match "*capa=r*" [$rd read]

        # Keyless read is redirected
        $rd DBSIZE
        assert_error "REDIRECT *" {$rd read}

        # Keyed read is redirected via standard cluster redirect
        $rd GET x
        assert_error "MOVED *" {$rd read}

        $rd close
    }
} ;# start_cluster
