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
} ;# start_cluster