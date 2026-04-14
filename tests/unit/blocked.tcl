tags {external:skip} {

test "DEBUG SLEEP ASYNC: basic unblock" {
    start_server {} {
        set r [srv 0 client]
        set result [$r DEBUG SLEEP 0.1 ASYNC]
        assert_equal OK $result
        set result [$r PING]
        assert_equal PONG $result
    }
}

test "DEBUG SLEEP ASYNC: multiple commands after unblock" {
    start_server {} {
        set r [srv 0 client]
        $r DEBUG SLEEP 0.05 ASYNC
        $r SET foo bar
        assert_equal bar [$r GET foo]
        $r DEBUG SLEEP 0.05 ASYNC
        assert_equal bar [$r GET foo]
    }
}

test "DEBUG SLEEP 0 ASYNC: synchronous unblock" {
    start_server {} {
        set r [srv 0 client]
        set result [$r DEBUG SLEEP 0 ASYNC]
        assert_equal OK $result
        set result [$r PING]
        assert_equal PONG $result
    }
}

} ;# tags
