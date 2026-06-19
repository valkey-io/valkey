start_server {tags {"service-time-latency"}} {
    test {Service time tracking disabled by default} {
        assert_equal [r config get latency-service-time-tracking] {latency-service-time-tracking no}
        r set a b
        r get a
        set histo [r latency histogram set get SERVICE]
        assert_equal [llength $histo] 0
    }

    test {Service time tracking can be enabled} {
        r config set latency-service-time-tracking yes
        assert_equal [r config get latency-service-time-tracking] {latency-service-time-tracking yes}
    }

    test {LATENCY HISTOGRAM SERVICE basic recording} {
        r config set latency-service-time-tracking yes
        r config resetstat
        r set a b
        r set c d
        r get a
        set histo [dict create {*}[r latency histogram set get SERVICE]]
        assert_match {calls 2 histogram_usec *} [dict get $histo set]
        assert_match {calls 1 histogram_usec *} [dict get $histo get]
    }

    test {LATENCY HISTOGRAM SERVICE specific commands} {
        r config set latency-service-time-tracking yes
        r config resetstat
        r set a b
        r set c d
        r get a
        r hset f k v
        set histo [dict create {*}[r latency histogram set hset SERVICE]]
        assert_match {calls 2 histogram_usec *} [dict get $histo set]
        assert_match {calls 1 histogram_usec *} [dict get $histo hset]
        assert_equal [dict size $histo] 2
    }

    test {LATENCY HISTOGRAM SERVICE does not affect processing time histogram} {
        r config set latency-service-time-tracking yes
        r config resetstat
        r set a b
        r get a
        # Processing time histogram (without SERVICE) should still work
        set proc_histo [dict create {*}[r latency histogram set get]]
        assert_match {calls 1 histogram_usec *} [dict get $proc_histo set]
        assert_match {calls 1 histogram_usec *} [dict get $proc_histo get]
        # Service time histogram should also have data
        set svc_histo [dict create {*}[r latency histogram set get SERVICE]]
        assert_match {calls 1 histogram_usec *} [dict get $svc_histo set]
        assert_match {calls 1 histogram_usec *} [dict get $svc_histo get]
    }

    test {LATENCY HISTOGRAM SERVICE with unknown command returns empty} {
        r config set latency-service-time-tracking yes
        r config resetstat
        r set a b
        set histo [r latency histogram blabla SERVICE]
        assert_equal [llength $histo] 0
    }

    test {LATENCY HISTOGRAM SERVICE all commands} {
        r config set latency-service-time-tracking yes
        r config resetstat
        r set a b
        r get a
        r hset f k v
        set histo [dict create {*}[r latency histogram SERVICE]]
        assert {[dict exists $histo set]}
        assert {[dict exists $histo get]}
        assert {[dict exists $histo hset]}
    }

    test {Service time is greater than or equal to processing time} {
        r config set latency-service-time-tracking yes
        r config resetstat
        r set a b
        set svc_histo [dict create {*}[r latency histogram set SERVICE]]
        set proc_histo [dict create {*}[r latency histogram set]]
        # Extract the max bucket from each (last value in histogram_usec)
        set svc_data [dict get [dict get $svc_histo set] histogram_usec]
        set proc_data [dict get [dict get $proc_histo set] histogram_usec]
        # Last bucket ceiling in service time should be >= processing time
        set svc_max [lindex $svc_data end-1]
        set proc_max [lindex $proc_data end-1]
        assert {$svc_max >= $proc_max}
    }

    test {Service time not recorded when tracking is disabled} {
        r config set latency-service-time-tracking yes
        r config resetstat
        r set a b
        # Verify data exists
        set histo [dict create {*}[r latency histogram set SERVICE]]
        assert_match {calls 1 histogram_usec *} [dict get $histo set]
        # Disable and send more commands
        r config set latency-service-time-tracking no
        r config resetstat
        r set a b
        r set c d
        set histo [r latency histogram set SERVICE]
        assert_equal [llength $histo] 0
    }

    test {Service time with MULTI/EXEC records once for EXEC} {
        r config set latency-service-time-tracking yes
        r config resetstat
        r multi
        r set a b
        r set c d
        r get a
        r exec
        # Sub-commands should not be recorded individually
        # Only exec should have a service time entry
        set histo [dict create {*}[r latency histogram exec SERVICE]]
        assert_match {calls 1 histogram_usec *} [dict get $histo exec]
    }

    test {Service time with pipeline records correct call counts} {
        r config set latency-service-time-tracking yes
        r config resetstat
        # Use a pipeline
        set rd [valkey_deferring_client]
        $rd write "*3\r\n\$3\r\nSET\r\n\$1\r\na\r\n\$1\r\nb\r\n"
        $rd write "*3\r\n\$3\r\nSET\r\n\$1\r\nc\r\n\$1\r\nd\r\n"
        $rd write "*2\r\n\$3\r\nGET\r\n\$1\r\na\r\n"
        $rd flush
        $rd read
        $rd read
        $rd read
        $rd close
        # Wait for write to complete
        after 100
        set histo [dict create {*}[r latency histogram set get SERVICE]]
        assert_match {calls 2 histogram_usec *} [dict get $histo set]
        assert_match {calls 1 histogram_usec *} [dict get $histo get]
    }

    test {LATENCY HISTOGRAM without SERVICE is unchanged} {
        r config set latency-service-time-tracking yes
        r config resetstat
        r set a b
        r get a
        # Default (no SERVICE keyword) returns processing time
        set histo [dict create {*}[r latency histogram set get]]
        assert_match {calls 1 histogram_usec *} [dict get $histo set]
        assert_match {calls 1 histogram_usec *} [dict get $histo get]
    }

    test {Error replies do not record service time} {
        r config set latency-service-time-tracking yes
        r config resetstat
        r set mykey myval
        # INCR on a string value will fail
        catch {r incr mykey}
        # The failed INCR should not appear in service time histogram
        set histo [r latency histogram incr SERVICE]
        assert_equal [llength $histo] 0
    }

    test {BLPOP service time includes blocked wait} {
        r config set latency-service-time-tracking yes
        r config resetstat
        r del mylist
        # Start a blocking pop in a separate client
        set rd [valkey_deferring_client]
        $rd blpop mylist 5
        $rd flush
        # Wait for client to block
        wait_for_condition 50 100 {
            [s blocked_clients] == 1
        } else {
            fail "Client not blocked"
        }
        # Wait 1 second then push to unblock
        after 1000
        r lpush mylist value
        # Read the BLPOP result — this ensures the write has completed
        set res [$rd read]
        $rd close
        after 100
        r ping
        # BLPOP is rewritten to LPOP internally when data is available,
        # so service time is recorded under lpop.
        set histo [dict create {*}[r latency histogram lpop SERVICE]]
        assert {[dict exists $histo lpop]}
        set data [dict get [dict get $histo lpop] histogram_usec]
        # Last bucket ceiling should be at least 500ms (500000 usec) — includes blocked wait
        set max_bucket [lindex $data end-1]
        assert {$max_bucket >= 500000}
    }

    test {BLPOP timeout does not record service time} {
        r config set latency-service-time-tracking yes
        r config resetstat
        r del mylist
        # BLPOP with 1 second timeout — will time out (nothing pushed)
        set rd [valkey_deferring_client]
        $rd blpop mylist 1
        wait_for_condition 50 100 {
            [s blocked_clients] == 1
        } else {
            fail "Client not blocked"
        }
        # Wait for timeout
        wait_for_condition 50 200 {
            [s blocked_clients] == 0
        } else {
            fail "Client still blocked after timeout"
        }
        $rd read
        $rd close
        after 100
        # Timed-out BLPOP returns nil — still gets recorded since it's a valid response
        set histo [dict create {*}[r latency histogram blpop SERVICE]]
        assert_match {calls 1 histogram_usec *} [dict get $histo blpop]
    }

    test {Config resetstat clears service time histograms} {
        r config set latency-service-time-tracking yes
        r set a b
        r get a
        set histo [dict create {*}[r latency histogram set get SERVICE]]
        assert {[dict size $histo] >= 2}
        r config resetstat
        set histo [r latency histogram set get SERVICE]
        assert_equal [llength $histo] 0
    }
}
