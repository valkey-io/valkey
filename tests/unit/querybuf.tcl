proc client_idle_sec {name} {
    set clients [split [r client list] "\r\n"]
    set c [lsearch -inline $clients *name=$name*]
    assert {[regexp {idle=([0-9]+)} $c - idle]}
    return $idle
}

# Calculate query buffer memory of client
proc client_query_buffer {name} {
    set clients [split [r client list] "\r\n"]
    set c [lsearch -inline $clients *name=$name*]
    if {[string length $c] > 0} {
        assert {[regexp {qbuf=([0-9]+)} $c - qbuf]}
        assert {[regexp {qbuf-free=([0-9]+)} $c - qbuf_free]}
        return [expr $qbuf + $qbuf_free]
    }
    return 0
}

start_server {tags {"querybuf slow"}} {
    # increase the execution frequency of clientsCron
    r config set hz 100

    # The test will run at least 2s to check if client query
    # buffer will be resized when client idle 2s.
    test "query buffer resized correctly" {

        set rd [valkey_deferring_client]

        $rd client setname test_client
        $rd read

        # Make sure query buff has size of 0 bytes at start as the client uses the shared qb.
        assert {[client_query_buffer test_client] == 0}

        # Pause cron to prevent premature shrinking (timing issue).
        r debug pause-cron 1

        # Send partial command to client to make sure it doesn't use the shared qb.
        $rd write "*3\r\n\$3\r\nset\r\n\$2\r\na"
        $rd flush
        # Wait for the client to start using a private query buffer. 
        wait_for_condition 1000 10 {
            [client_query_buffer test_client] > 0
        } else {
            fail "client should start using a private query buffer"
        }
     
        # send the rest of the command
        $rd write "a\r\n\$1\r\nb\r\n"
        $rd flush
        assert_equal {OK} [$rd read]

        set orig_test_client_qbuf [client_query_buffer test_client]
        # Make sure query buff has less than the peak resize threshold (PROTO_RESIZE_THRESHOLD) 32k
        # but at least the basic IO reading buffer size (PROTO_IOBUF_LEN) 16k
        set MAX_QUERY_BUFFER_SIZE [expr 32768 + 2] ; # 32k + 2, allowing for potential greedy allocation of (16k + 1) * 2 bytes for the query buffer.
        assert {$orig_test_client_qbuf >= 16384 && $orig_test_client_qbuf <= $MAX_QUERY_BUFFER_SIZE}

        # Allow shrinking to occur
        r debug pause-cron 0

        # Check that the initial query buffer is resized after 2 sec
        wait_for_condition 1000 10 {
            [client_idle_sec test_client] >= 3 && [client_query_buffer test_client] < $orig_test_client_qbuf
        } else {
            fail "query buffer was not resized"
        }
        $rd close
    }

    test "large bulk arguments release the query buffer after parsing" {
        set rd [valkey_client]
        $rd client setname big_arg_client

        set value [string repeat A 400000]
        for {set i 0} {$i < 3} {incr i} {
            assert_equal OK [$rd set "large:$i" $value]
            assert_equal $value [r get "large:$i"]
            assert {[client_query_buffer big_arg_client] < 32768}
        }

        $rd write "*2\r\n\$4\r\nPING\r\n\$4\r\nPONG\r\n"
        $rd write "*2\r\n\$4\r\nPING\r\n\$4\r\nDONE\r\n"
        $rd flush
        assert_equal PONG [$rd read]
        assert_equal DONE [$rd read]
        $rd close
    }

    test "query buffer resized correctly with fat argv" {
        set rd [valkey_client]
        $rd client setname test_client
        $rd write "*3\r\n\$3\r\nset\r\n\$1\r\na\r\n\$1000000\r\n"
        $rd flush
        
        after 200
        # Send the start of the arg and make sure the client is not using shared qb for it rather a private buf of > 1000000 size.
        $rd write "a" 
        $rd flush

        after 20
        if {[client_query_buffer test_client] < 1000000} {
            fail "query buffer should not be resized when client idle time smaller than 2s"
        }
     
        # Check that the query buffer is resized after 2 sec
        wait_for_condition 1000 10 {
            [client_idle_sec test_client] >= 3 && [client_query_buffer test_client] < 1000000
        } else {
            fail "query buffer should be resized when client idle time bigger than 2s"
        }
     
        $rd close
    }

}
