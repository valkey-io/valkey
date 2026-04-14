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

    test {keyless read commands execute on replica without redirect-keyless capa} {
        # Without the capa, keyless read commands like SCAN execute locally on the replica
        # After failover, node 0 is the new replica.
        set rd [valkey_deferring_client 0]
        $rd SCAN 0
        set reply [$rd read]
        assert_match "0 *" $reply
        $rd close
    }

    test {keyless read commands are MOVED with redirect-keyless capa} {
        set rd [valkey_deferring_client 0]
        $rd CLIENT CAPA redirect-keyless
        assert_equal OK [$rd read]

        $rd DBSIZE
        assert_error "MOVED *" {$rd read}

        $rd RANDOMKEY
        assert_error "MOVED *" {$rd read}

        $rd SCAN 0
        assert_error "MOVED *" {$rd read}

        $rd close
    }

    test {keyless read commands execute on replica with redirect-keyless and READONLY} {
        set rd [valkey_deferring_client 0]
        $rd CLIENT CAPA redirect-keyless
        assert_equal OK [$rd read]
        $rd READONLY
        assert_equal OK [$rd read]

        # With READONLY, keyless reads should execute locally
        $rd DBSIZE
        set reply [$rd read]
        assert {$reply >= 0}

        $rd close
    }

    test {non-read keyless commands are not affected by redirect-keyless capa} {
        set rd [valkey_deferring_client 0]
        $rd CLIENT CAPA redirect-keyless
        assert_equal OK [$rd read]

        # PING is not CMD_READONLY, should still work on replica
        $rd PING
        assert_equal PONG [$rd read]

        $rd close
    }

    test {CLIENT INFO reports redirect-keyless capa} {
        set rd [valkey_deferring_client 0]
        $rd CLIENT CAPA redirect-keyless
        assert_equal OK [$rd read]

        $rd CLIENT INFO
        assert_match "*capa=k*" [$rd read]

        $rd close
    }
} ;# start_cluster