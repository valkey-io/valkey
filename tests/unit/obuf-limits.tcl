start_server {tags {"obuf-limits external:skip logreqres:skip"}} {
    # Disable copy avoidance because it affects memory usage
    r config set min-io-threads-avoid-copy-reply 0

    test {CONFIG SET client-output-buffer-limit} {
        set oldval [lindex [r config get client-output-buffer-limit] 1]

        catch {r config set client-output-buffer-limit "wrong number"} e
        assert_match {*Wrong*arguments*} $e

        catch {r config set client-output-buffer-limit "invalid_class 10mb 10mb 60"} e
        assert_match {*Invalid*client*class*} $e
        catch {r config set client-output-buffer-limit "master 10mb 10mb 60"} e
        assert_match {*Invalid*client*class*} $e

        catch {r config set client-output-buffer-limit "normal 10mbs 10mb 60"} e
        assert_match {*Error*hard*} $e

        catch {r config set client-output-buffer-limit "replica 10mb 10mbs 60"} e
        assert_match {*Error*soft*} $e

        catch {r config set client-output-buffer-limit "pubsub 10mb 10mb 60s"} e
        assert_match {*Error*soft_seconds*} $e

        r config set client-output-buffer-limit "normal 1mb 2mb 60 replica 3mb 4mb 70 pubsub 5mb 6mb 80"
        set res [lindex [r config get client-output-buffer-limit] 1]
        assert_equal $res "normal 1048576 2097152 60 slave 3145728 4194304 70 pubsub 5242880 6291456 80"

        # Set back to the original value.
        r config set client-output-buffer-limit $oldval
    }

    test {Client output buffer hard limit is enforced} {
        r config set client-output-buffer-limit {pubsub 100000 0 0}
        set rd1 [valkey_deferring_client]

        $rd1 subscribe foo
        set reply [$rd1 read]
        assert {$reply eq "subscribe foo 1"}

        set omem 0
        while 1 {
            # The larger content size ensures that client.buf gets filled more quickly,
            # allowing us to correctly observe the gradual increase of `omem`
            r publish foo [string repeat bar 50]
            set clients [split [r client list] "\r\n"]
            set c [split [lindex $clients 1] " "]
            if {![regexp {omem=([0-9]+)} $c - omem]} break
            if {$omem > 200000} break
        }
        assert {$omem >= 70000 && $omem < 200000}
        $rd1 close
    }
    
    foreach {soft_limit_time wait_for_timeout} {3 yes
                                                4 no } {
        if $wait_for_timeout {
            set test_name "Client output buffer soft limit is enforced if time is overreached"
        } else {
            set test_name "Client output buffer soft limit is not enforced too early and is enforced when no traffic"
        }

        test $test_name {
            r config set client-output-buffer-limit "pubsub 0 100000 $soft_limit_time"
            set soft_limit_time [expr $soft_limit_time*1000]
            set rd1 [valkey_deferring_client]

            $rd1 client setname test_client
            set reply [$rd1 read]
            assert {$reply eq "OK"}

            $rd1 subscribe foo
            set reply [$rd1 read]
            assert {$reply eq "subscribe foo 1"}

            set omem 0
            set start_time 0
            set time_elapsed 0
            set last_under_limit_time [clock milliseconds]
            while 1 {
                r publish foo [string repeat "x" 1000]
                set clients [split [r client list] "\r\n"]
                set c [lsearch -inline $clients *name=test_client*]
                if {$start_time != 0} {
                    set time_elapsed [expr {[clock milliseconds]-$start_time}]
                    # Make sure test isn't taking too long
                    assert {$time_elapsed <= [expr $soft_limit_time+3000]}
                }
                if {$wait_for_timeout && $c == ""} {
                    # Make sure we're disconnected when we reach the soft limit
                    assert {$omem >= 100000 && $time_elapsed >= $soft_limit_time}
                    break
                } else {
                    assert {[regexp {omem=([0-9]+)} $c - omem]}
                }
                if {$omem > 100000} {
                    if {$start_time == 0} {set start_time $last_under_limit_time}
                    if {!$wait_for_timeout && $time_elapsed >= [expr $soft_limit_time-1000]} break
                    # Slow down loop when omem has reached the limit.
                    after 10
                } else {
                    # if the OS socket buffers swallowed what we previously filled, reset the start timer.
                    set start_time 0
                    set last_under_limit_time [clock milliseconds]
                }
            }

            if {!$wait_for_timeout} {
                # After we completely stopped the traffic, wait for soft limit to time out
                set timeout [expr {$soft_limit_time+1500 - ([clock milliseconds]-$start_time)}]
                wait_for_condition [expr $timeout/10] 10 {
                    [lsearch [split [r client list] "\r\n"] *name=test_client*] == -1
                } else {
                    fail "Soft limit timed out but client still connected"
                }
            }

            $rd1 close
        }
    }

    test {No response for single command if client output buffer hard limit is enforced} {
        r config set latency-tracking no
        r config set client-output-buffer-limit {normal 100000 0 0}
        # Total size of all items must be more than 100k
        set item [string repeat "x" 1000]
        for {set i 0} {$i < 150} {incr i} {
            r lpush mylist $item
        }
        set orig_mem [s used_memory]
        # Set client name and get all items
        set rd [valkey_deferring_client]
        $rd client setname mybiglist
        assert {[$rd read] eq "OK"}
        $rd lrange mylist 0 -1
        $rd flush
        after 100

        # Before we read reply, the server will close this client.
        set clients [r client list]
        assert_no_match "*name=mybiglist*" $clients
        set cur_mem [s used_memory]
        # 10k just is a deviation threshold
        assert {$cur_mem < 10000 + $orig_mem}

        # Read nothing
        set fd [$rd channel]
        assert_equal {} [$rd rawread]
    }

    # Note: This test assumes that what's written with one write, will be read by the server in one read.
    # this assumption is wrong, but seem to work empirically (for now)
    test {No response for multi commands in pipeline if client output buffer limit is enforced} {
        r config set client-output-buffer-limit {normal 100000 0 0}
        set value [string repeat "x" 10000]
        r set bigkey $value
        set rd1 [valkey_deferring_client]
        set rd2 [valkey_deferring_client]
        $rd2 client setname multicommands
        assert_equal "OK" [$rd2 read]

        # Let the server sleep 1s firstly
        $rd1 debug sleep 1
        $rd1 flush
        after 100

        # Create a pipeline of commands that will be processed in one socket read.
        # It is important to use one write, in TLS mode independent writes seem
        # to wait for response from the server.
        # Total size should be less than OS socket buffer, the server can
        # execute all commands in this pipeline when it wakes up.
        set buf ""
        for {set i 0} {$i < 15} {incr i} {
            append buf "set $i $i\r\n"
            append buf "get $i\r\n"
            append buf "del $i\r\n"
            # One bigkey is 10k, total response size must be more than 100k
            append buf "get bigkey\r\n"
        }
        $rd2 write $buf
        $rd2 flush
        after 100

        # Reds must wake up if it can send reply
        assert_equal "PONG" [r ping]
        set clients [r client list]
        assert_no_match "*name=multicommands*" $clients
        assert_equal {} [$rd2 rawread]
    }

    test {Execute transactions completely even if client output buffer limit is enforced} {
        r config set client-output-buffer-limit {normal 100000 0 0}
        # Total size of all items must be more than 100k
        set item [string repeat "x" 1000]
        for {set i 0} {$i < 150} {incr i} {
            r lpush mylist2 $item
        }

        # Output buffer limit is enforced during executing transaction
        r client setname transactionclient
        r set k1 v1
        r multi
        r set k2 v2
        r get k2
        r lrange mylist2 0 -1
        r set k3 v3
        r del k1
        catch {[r exec]} e
        assert_match "*I/O error*" $e
        reconnect
        set clients [r client list]
        assert_no_match "*name=transactionclient*" $clients

        # Transactions should be executed completely
        assert_equal {} [r get k1]
        assert_equal "v2" [r get k2]
        assert_equal "v3" [r get k3]
    }

    test "Obuf limit, HRANDFIELD with huge count stopped mid-run" {
        r config set client-output-buffer-limit {normal 1000000 0 0}
        r hset myhash a b
        catch {r hrandfield myhash -999999999} e
        assert_match "*I/O error*" $e
        reconnect
    }

    test "Obuf limit, KEYS stopped mid-run" {
        r config set client-output-buffer-limit {normal 100000 0 0}
        populate 1000 "long-key-name-prefix-of-100-chars-------------------------------------------------------------------"
        catch {r keys *} e
        assert_match "*I/O error*" $e
        reconnect
    }

    test {Obuf hard limit with copy avoidance enabled} {
        # Enable copy avoidance
        r config set min-io-threads-avoid-copy-reply 1
        r config set client-output-buffer-limit {normal 200000 0 0}
        
        # Create large value (1MB each)
        set value [string repeat "x" [expr 1*1024*1024]]
        r set bigkey $value
        
        set rd [valkey_deferring_client]
        $rd client setname copy_avoid_hard
        assert {[$rd read] eq "OK"}
        
        # Send GET commands without reading responses
        # This fills the output buffer faster than socket can drain
        set omem 0
        while {1} {
            $rd get bigkey
            $rd flush
            after 10
            set clients [r client list]
            set found 0
            foreach client_info [split $clients "\r\n"] {
                if {[string match "*name=copy_avoid_hard*" $client_info]} {
                    regexp {omem=([0-9]+)} $client_info _ omem
                    set found 1
                    break
                }
            }
            if {$omem >= 200000} break
            if {!$found} break
        }
        
        wait_for_condition 50 100 {
            [lsearch [split [r client list] "\r\n"] *name=copy_avoid_hard*] == -1
        } else {
            fail "Client not disconnected despite omem=$omem >= 200000"
        }
    }

    test {Obuf soft limit with copy avoidance enabled} {
        r config set client-output-buffer-limit {normal 0 150000 2}
        
        # Use 500KB value
        set value [string repeat "y" [expr 500*1024]]
        r set mediumkey $value
        
        set rd [valkey_deferring_client]
        $rd client setname copy_avoid_soft
        assert {[$rd read] eq "OK"}
        
        # Send GETs to exceed soft limit
        # With copy avoidance, tracking happens async in I/O threads
        set omem 0
        set extra_sends 0
        while {1} {
            $rd get mediumkey
            $rd flush
            after 10
            set clients [r client list]
            set found 0
            foreach client_info [split $clients "\r\n"] {
                if {[string match "*name=copy_avoid_soft*" $client_info]} {
                    regexp {omem=([0-9]+)} $client_info _ omem
                    set found 1
                    break
                }
            }
            if {!$found} break
            
            if {$omem >= 150000} {
                incr extra_sends
                if {$extra_sends >= 5} break
            }
        }
        
        assert {[lsearch [split [r client list] "\r\n"] *name=copy_avoid_soft*] != -1}
        
        wait_for_condition 50 100 {
            [lsearch [split [r client list] "\r\n"] *name=copy_avoid_soft*] == -1
        } else {
            fail "Client not disconnected after soft limit timeout (omem=$omem)"
        }
    }

    test {Copy avoidance obuf tracking with IO threads} {
        # Enable copy avoidance and IO threads
        r config set min-io-threads-avoid-copy-reply 1
        r config set io-threads 4
        r config set client-output-buffer-limit {normal 200000 0 0}
        
        # Use 1MB value
        set value [string repeat "w" [expr 1*1024*1024]]
        r set iothread_key $value
        
        set rd [valkey_deferring_client]
        $rd client setname iothread_test
        assert {[$rd read] eq "OK"}
        
        # Send multiple GETs without reading
        set omem 0
        while {1} {
            $rd get iothread_key
            $rd flush
            after 10
            set clients [r client list]
            foreach client_info [split $clients "\r\n"] {
                if {[string match "*name=iothread_test*" $client_info]} {
                    regexp {omem=([0-9]+)} $client_info _ omem
                    break
                }
            }
            if {$omem >= 200000} break
            if {[lsearch [split [r client list] "\r\n"] *name=iothread_test*] == -1} break
        }
        
        # Wait for disconnection
        wait_for_condition 50 100 {
            [lsearch [split [r client list] "\r\n"] *name=iothread_test*] == -1
        } else {
            fail "Client not disconnected with IO threads (omem=$omem)"
        }
        
        # Restore settings
        r config set min-io-threads-avoid-copy-reply 0
        r config set io-threads 1
    }

    test {Copy avoidance spill to reply list returns omem to zero after drain} {
        r config set min-io-threads-avoid-copy-reply 1
        r config set io-threads 4
        r config set commandlog-reply-larger-than 1
        r config set client-output-buffer-limit {normal 0 0 0}

        set value [string repeat "q" [expr 16*1024]]
        r set spill_key $value

        set rd [valkey_deferring_client]
        $rd client setname spill_omem_test
        assert_equal "OK" [$rd read]
        $rd client id
        set client_id [$rd read]

       
        # Each pipelined GET for a 16 KB value requires payloadHeader (20 bytes on 64-bit)
        # and bulkStrRef (16 bytes) in client buf (PROTO_REPLY_CHUNK_BYTES = 16 KB).
        # 1300 commands * ~36 bytes/header >> 16 KB, so replies spill into c->reply list.
        # On 32-bit header size is smaller, so we need 3000 commands * ~20 bytes/header >> 16 KB.
        if {[s arch_bits] == 64} {
            set cmd_count 1300
        } else {
            set cmd_count 3000
        }
        set pipeline ""
        for {set i 0} {$i < $cmd_count} {incr i} {
            append pipeline "get spill_key\r\n"
        }
        $rd write $pipeline
        $rd flush

        set spilled_to_reply_list 0
        for {set i 0} {$i < 100} {incr i} {
            set oll [get_field_in_client_list $client_id [r client list] oll]
            if {$oll ne "" && $oll > 0} {
                set spilled_to_reply_list 1
                break
            }
            after 50
        }
        if {!$spilled_to_reply_list} {
            fail "Client never spilled copy-avoided replies into c->reply"
        }

        set reply_len [expr {[string length $value] + [string length [string length $value]] + 5}]
        set remaining [expr {$reply_len * $cmd_count}]
        while {$remaining > 0} {
            set chunk [$rd rawread [expr {min($remaining, 65536)}]]
            set chunk_len [string length $chunk]
            if {$chunk_len == 0} {
                fail "Socket drained unexpectedly after reading [expr {$reply_len * $cmd_count - $remaining}] bytes"
            }
            incr remaining -$chunk_len
        }

        set fully_drained 0
        for {set i 0} {$i < 100} {incr i} {
            set client_list [r client list]
            set obl [get_field_in_client_list $client_id $client_list obl]
            set oll [get_field_in_client_list $client_id $client_list oll]
            if {$obl ne "" && $oll ne "" && $obl == 0 && $oll == 0} {
                set fully_drained 1
                break
            }
            after 50
        }
        if {!$fully_drained} {
            fail "Client reply buffers did not fully drain"
        }

        set omem [get_field_in_client_list $client_id [r client list] omem]

        $rd close
        r config set commandlog-reply-larger-than -1
        r config set min-io-threads-avoid-copy-reply 0
        r config set io-threads 1

        assert_equal 0 $omem
    }
}
