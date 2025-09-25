
set testmodule [file normalize tests/modules/block_keyspace_notification.so]

start_server {tags {"modules"}} {
    r module load $testmodule
    test {Blocking keyspace notification one} {
        wait_for_blocked_clients_count 0
        r b_keyspace.clear
        assert_equal "1" [r hset a b c]
        assert_equal "{event hset key a}" [r b_keyspace.events]
    }
    test {Blocking keyspace notification two} {
        wait_for_blocked_clients_count 0
        r b_keyspace.clear
        set rd1 [valkey_deferring_client]
        $rd1 hset b c d
        after 500
        set rd2 [valkey_deferring_client]
        $rd2 hset c d e
        wait_for_blocked_clients_count 2
        assert_equal "" [r b_keyspace.events]
        wait_for_blocked_clients_count 1
        assert_equal "{event hset key b}" [r b_keyspace.events]
        wait_for_blocked_clients_count 0
        assert_equal "{event hset key b} {event hset key c}" [r b_keyspace.events]
    }
    test {Blocking keyspace notification with pipelining hset after hget} {
        wait_for_blocked_clients_count 0
        r b_keyspace.clear
        set rd1 [valkey_deferring_client]
        $rd1 hset key_10 field_10 value_10
        wait_for_blocked_clients_count 0
        assert_equal "1" [$rd1 read]
        pause_process [srv 0 pid]
        # Queue up three commands
        $rd1 hget key_10 field_10
        $rd1 hset key_10 value_10 ss
        $rd1 hget key_10 field_10
        set start [clock milliseconds]; # Record time before unblocking
        resume_process [srv 0 pid]
        assert_equal "value_10" [$rd1 read]
        set first_hget [expr [clock milliseconds] - $start];
        assert_equal "1" [$rd1 read]
        set first_hset [expr [clock milliseconds] - $start];
        assert_equal "value_10" [$rd1 read]
        set second_hget [expr [clock milliseconds] - $start];
        assert {[expr {$first_hget * 10 < $first_hset}]}
        assert {[expr {$second_hget - $first_hset <= 50}]}
        wait_for_blocked_clients_count 0
    }
    test {Blocking keyspace notification with pipelining hget after hset} {
        wait_for_blocked_clients_count 0
        r b_keyspace.clear
        set rd1 [valkey_deferring_client]
        pause_process [srv 0 pid]
        $rd1 hset key_2 field_1 value_1
        $rd1 hget key_2 field_1
        resume_process [srv 0 pid]
        wait_for_blocked_clients_count 1
        assert_equal "" [r b_keyspace.events]
        wait_for_blocked_clients_count 0
        assert_equal "{event hset key key_2}" [r b_keyspace.events]
        assert_equal "1" [$rd1 read]
        assert_equal "value_1" [$rd1 read]
    }
    test {Blocking keyspace notif during multi} {
        wait_for_blocked_clients_count 0
        r b_keyspace.clear
        r multi
        r hset d e f
        r hset e f g
        assert_equal "1 1" [r exec]
        # Nothing should be blocked inside the transaction
        assert_equal [s 0 blocked_clients] 0
        assert_equal [r b_keyspace.events] ""
        # In the background, the work should still occur
        wait_for_condition 1000 50 {
            [string match "{event hset key *} {event hset key *}" [set latest [r b_keyspace.events]]]
        } else {
            fail "Keyspace event not propagated within 5 seconds"
        }
    }
    test {Event that fires twice} {
        wait_for_blocked_clients_count 0
        r b_keyspace.clear
        r hset f g h
        wait_for_blocked_clients_count 0
        assert_equal "OK" [r b_keyspace.clear]
        set rd1 [valkey_deferring_client]
        $rd1 RENAME f g
        # Only one blocked client
        assert_equal [s 0 blocked_clients] 1
        assert_equal "" [r b_keyspace.events]
        # We should still see async processing for both events, which could be processed in either order
        wait_for_condition 1000 50 {
            [string match "{event rename_* key *} {event rename_* key *}" [set latest [r b_keyspace.events]]]
        } else {
            fail "Rename event not propagated within 5 seconds: latest value ${latest}"
        }
    }
    test {Server-created keyspace notification} {
        wait_for_blocked_clients_count 0
        r b_keyspace.clear
        assert_equal "1" [r hset h i j]
        assert_equal "1" [r expire h 1]
        assert_equal [s 0 blocked_clients] 0
        # Expire will proceed processing in the background as we haven't
        # enabled blocking for the command yet.
        wait_for_condition 1000 50 {
            [r b_keyspace.events] eq "{event hset key h} {event expire key h}"
        } else {
            fail "Expire event not propagated within 5 seconds"
        }
        assert_equal "OK" [r b_keyspace.clear]
        # Expiration should happen in the background
        wait_for_condition 1000 50 {
            [r b_keyspace.events] eq "{event expired key h}"
        } else {
            fail "Expired event not propagated within 5 seconds"
        }
    }
    test "Unload the module - testblockingkeyspacenotif" {
        assert_equal {OK} [r module unload testblockingkeyspacenotif]
    }
}

