# Reply blocking must track every key a module modifies, not only the keys a
# command declares through keyspecs. A read of such a key has to wait until
# the write that produced it is durable.

set rbwmodule [file normalize tests/modules/reply_blocking_writes.so]

start_server {tags {"modules durability external:skip"} overrides {appendonly yes appendfsync always bio-aof-offload-enabled yes}} {
    r module load $rbwmodule

    # Issue a read on a fresh connection and assert its reply is withheld
    # while the AOF is paused, then released once it resumes.
    # waiting is the number of clients expected on the waiting list once the
    # read is held (a writer whose own reply is held counts too).
    proc assert_read_held_until_resume {cmd expected waiting} {
        set rd [valkey_deferring_client]
        $rd {*}$cmd
        wait_for_condition 50 100 {
            [string match "*reply_blocking_clients_waiting_ack:$waiting*" [r info debug]]
        } else {
            r debug reply-blocking-resume aof
            fail "read '$cmd' was not held for durability"
        }
        # Give a leaked reply time to arrive before checking for it.
        after 100
        set fd [$rd channel]
        fconfigure $fd -blocking 0
        assert_equal "" [read $fd]
        fconfigure $fd -blocking 1

        r debug reply-blocking-resume aof
        r ping
        assert_equal $expected [$rd read]
        $rd close
    }

    test "Read of a key written by VM_Call from a module timer waits for durability" {
        r del rbw:timer
        r debug reply-blocking-pause aof
        r rbw.timerset rbw:timer t1
        wait_for_condition 50 100 {
            [catch {r debug object rbw:timer}] == 0
        } else {
            r debug reply-blocking-resume aof
            fail "timer did not fire"
        }
        assert_read_held_until_resume {get rbw:timer} t1 1
    }

    test "Read of a key written by VM_Call from a keyless module command waits for durability" {
        r debug reply-blocking-pause aof
        set writer [valkey_deferring_client]
        $writer rbw.callset rbw:callset v1
        wait_for_condition 50 100 {
            [catch {r debug object rbw:callset}] == 0
        } else {
            r debug reply-blocking-resume aof
            fail "write was not applied"
        }
        assert_read_held_until_resume {get rbw:callset} v1 2
        assert_equal OK [$writer read]
        $writer close
    }

    test "Read of a key written through the low-level key API waits for durability" {
        r debug reply-blocking-pause aof
        set writer [valkey_deferring_client]
        $writer rbw.keyset rbw:keyset v2
        wait_for_condition 50 100 {
            [catch {r debug object rbw:keyset}] == 0
        } else {
            r debug reply-blocking-resume aof
            fail "write was not applied"
        }
        assert_read_held_until_resume {get rbw:keyset} v2 2
        assert_equal OK [$writer read]
        $writer close
    }
}
