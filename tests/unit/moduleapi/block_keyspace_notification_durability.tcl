# A module that blocks the client from its keyspace-notification callback
# (upstream PR #1819) combined with reply-blocking durability
# (bio-aof-offload-enabled + appendfsync always).
#
# The test module blocks the client on the "hset" event; a background worker
# logs the event and unblocks the client after ~1s. With both features on, a
# write reply must be released only after BOTH the module unblocks AND the write
# is durably acknowledged.

set testmodule [file normalize tests/modules/block_keyspace_notification.so]

start_server {tags {"modules durability"} overrides {appendonly yes appendfsync always bio-aof-offload-enabled yes}} {
    r module load $testmodule

    test "module block on a keyspace event withholds the write reply until unblock" {
        wait_for_blocked_clients_count 0
        r b_keyspace.clear

        set rd [valkey_deferring_client]
        $rd hset on:a f v

        # Client is blocked by the module; reply not delivered yet.
        wait_for_blocked_clients_count 1
        set fd [$rd channel]
        fconfigure $fd -blocking 0
        assert_equal "" [read $fd]
        fconfigure $fd -blocking 1

        # Worker unblocks (~1s); reply released and the event already processed.
        assert_equal 1 [$rd read]
        $rd close
        assert_equal "{event hset key on:a}" [r b_keyspace.events]
        assert_equal v [r hget on:a f]
    }

    test "module-blocked write reply waits for the durability ack, not just unblock" {
        # With the provider paused, the reply must stay withheld even after the
        # module unblocks, and be released only once durability acks on resume.
        # (The durability suite separately covers that the pause lever withholds.)
        wait_for_blocked_clients_count 0
        r b_keyspace.clear
        r DEBUG reply-blocking-pause aof

        set rd [valkey_deferring_client]
        $rd hset dur:bug f v

        wait_for_blocked_clients_count 1
        set fd [$rd channel]
        fconfigure $fd -blocking 0
        assert_equal "" [read $fd]
        fconfigure $fd -blocking 1

        # Module unblocks (~1s) but durability is still paused -> still withheld.
        wait_for_blocked_clients_count 0
        after 300
        fconfigure $fd -blocking 0
        assert_equal "" [read $fd]
        fconfigure $fd -blocking 1

        r DEBUG reply-blocking-resume aof
        r ping
        assert_equal 1 [$rd read]
        $rd close
        assert_equal v [r hget dur:bug f]
    }

    test "modules are notified on writes even when notify-keyspace-events is empty" {
        # Module subscribers filter by their own event mask, so an empty
        # notify-keyspace-events must not stop them from receiving events.
        r config set notify-keyspace-events ""
        wait_for_blocked_clients_count 0
        r b_keyspace.clear
        r set plain:a v
        wait_for_condition 50 100 {
            [r b_keyspace.events] eq "{event set key plain:a}"
        } else {
            fail "module keyspace event dropped under empty notify-keyspace-events"
        }
    }

    test "pipelined read freed during a module block does not corrupt the held reply" {
        # A large GET reply sits in c->reply when the pipelined, module-blocking
        # HSET runs; it is flushed and freed while the client is blocked, then the
        # HSET reply is committed on unblock. The reply-blocking boundary must be
        # measured against the buffer state at commit time, not reused from when
        # the command started (which would reference the freed node). The
        # use-after-free is caught deterministically only under AddressSanitizer.
        wait_for_blocked_clients_count 0
        r b_keyspace.clear

        # Large value so the GET reply overflows the static buffer into c->reply.
        set big [string repeat x 100000]
        r set k1 $big

        set rd [valkey_deferring_client]
        # Pipeline a clean-key read and the blocking write into one pass.
        pause_process [srv 0 pid]
        $rd get k1
        $rd hset k2 f v
        resume_process [srv 0 pid]

        # GET reply flushes here, freeing its c->reply node during the block.
        assert_equal $big [$rd read]

        wait_for_blocked_clients_count 1
        assert_equal 1 [$rd read]
        $rd close

        # Server healthy and write applied (no crash / corruption).
        assert_equal PONG [r ping]
        assert_equal v [r hget k2 f]
    }

    test "Unload the module - testblockingkeyspacenotif" {
        assert_equal {OK} [r module unload testblockingkeyspacenotif]
    }
}
