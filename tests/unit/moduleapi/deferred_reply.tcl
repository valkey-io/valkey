set testmodule [file normalize tests/modules/deferred_reply.so]

start_server {tags {"modules logreqres:skip"}} {
    r module load $testmodule

    test {Deferred reply with postponed array length during active deferred reply buffer} {
        r deferred_reply.arm

        set rd [valkey_deferring_client]
        # SET triggers NOTIFY_STRING.  The module callback blocks the client;
        # the reply callback builds a nested array using POSTPONED_LEN while
        # the deferred reply buffer is active.
        $rd set testkey testval

        wait_for_blocked_clients_count 1
        wait_for_blocked_clients_count 0

        set set_reply [$rd read]
        set cb_reply [$rd read]

        # Drain any extra data so we can report it on failure.
        $rd ping diag
        set extra {}
        set tok [$rd read]
        while {$tok ne "diag"} {
            lappend extra $tok
            set tok [$rd read]
        }

        if {$set_reply ne "OK" || $cb_reply ne "first {a b}" || $extra ne {}} {
            fail "Malformed deferred reply output: set_reply='$set_reply' cb_reply='$cb_reply' extra='$extra'"
        }

        $rd close
    }

    test "Unload the module - deferred_reply" {
        assert_equal {OK} [r module unload deferred_reply]
    }
}
