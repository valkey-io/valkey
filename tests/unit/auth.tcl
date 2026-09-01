start_server {tags {"auth external:skip"}} {
    test {AUTH fails if there is no password configured server side} {
        catch {r auth foo} err
        set _ $err
    } {ERR *any password*}

    test {Arity check for auth command} {
        catch {r auth a b c} err
        set _ $err
    } {*syntax error*}
}

start_server {tags {"auth external:skip"} overrides {requirepass foobar}} {
    test {AUTH fails when a wrong password is given} {
        catch {r auth wrong!} err
        set _ $err
    } {WRONGPASS*}

    test {Arbitrary command gives an error when AUTH is required} {
        catch {r set foo bar} err
        set _ $err
    } {NOAUTH*}

    test {AUTH succeeds when the right password is given} {
        r auth foobar
    } {OK}

    test {Once AUTH succeeded we can actually send commands to the server} {
        r set foo 100
        r incr foo
    } {101}

    test {For unauthenticated clients multibulk and bulk length are limited} {
        set rr [valkey [srv "host"] [srv "port"] 0 $::tls]
        $rr write "*100\r\n"
        $rr flush
        catch {[$rr read]} e
        assert_match {*unauthenticated multibulk length*} $e
        $rr close

        set rr [valkey [srv "host"] [srv "port"] 0 $::tls]
        $rr write "*1\r\n\$100000000\r\n"
        $rr flush
        catch {[$rr read]} e
        assert_match {*unauthenticated bulk length*} $e
        $rr close
    }

    test {For unauthenticated clients output buffer is limited} {
        set rr [valkey [srv "host"] [srv "port"] 1 $::tls]
        
        # make sure the client is no longer authenticated
        $rr SET x 5
        catch {[$rr read]} e
        assert_match {*NOAUTH Authentication required*} $e

        # Force clients to use the reply list in order 
        # to make them have a larger than 1K output buffer on any reply
        assert_match "OK" [r debug client-enforce-reply-list 1]
        
        # Fill the output buffer without reading it and make
        # sure the client disconnected.
        $rr SET x 5
        catch {[$rr read]} e
        assert_match {I/O error reading reply} $e  

        # Reset the debug
        assert_match "OK" [r debug client-enforce-reply-list 0]
    }  

    test {For once authenticated clients output buffer is NOT limited} {
        set rr [valkey [srv "host"] [srv "port"] 1 $::tls]

        # first time authenticate client
        $rr auth foobar
        assert_equal {OK} [$rr read]

        # now reset the client so it is not authenticated
        $rr reset
        assert_equal {RESET} [$rr read]

        # make sure the client is no longer authenticated
        $rr SET x 5
        catch {[$rr read]} e
        assert_match {*NOAUTH Authentication required*} $e

        # Force clients to use the reply list in order 
        # to make them have a larger than 1K output buffer on any reply
        assert_match "OK" [r debug client-enforce-reply-list 1]
        
        # Fill the output buffer without reading it and make
        # sure the client was NOT disconnected.

        $rr SET x 5
        catch {[$rr read]} e
        assert_match {*NOAUTH Authentication required*} $e

        # Reset the debug
        assert_match "OK" [r debug client-enforce-reply-list 0]
    }

    test {Unauthenticated multibulk limit works in pipeline after AUTH} {
        # Checks that the parser doesn't parse multiple commands when the client
        # is unauthenticated but becomes authenticated within the same pipeline.

        # To make sure the pipeline is received as a single packet, send it as a
        # single packet in raw RESP form.

        # AUTH foobar
        set auth_proto "*2\r\n\$4\r\nAUTH\r\n\$6\r\nfoobar\r\n"

        # SET A X
        set set_proto "*3\r\n\$3\r\nSET\r\n\$1\r\nA\r\n\$1\r\nX\r\n"

        # MGET A A A A A A A A A A (too many args for unauthenticated user)
        set A "\$1\r\nA\r\n"
        set mget_proto "*11\r\n\$4\r\nMGET\r\n$A$A$A$A$A$A$A$A$A$A"

        set proto "$auth_proto$set_proto$mget_proto"
        set rd [valkey [srv "host"] [srv "port"] 1 $::tls]
        set fd [$rd channel]
        puts -nonewline $fd $proto
        flush $fd

        assert_equal OK [$rd read]
        assert_equal OK [$rd read]
        assert_equal {X X X X X X X X X X} [$rd read]
        $rd close
    }
}

foreach dualchannel {yes no} {
start_server {tags {"auth_binary_password external:skip"}} {
    test {AUTH fails when binary password is wrong} {
        r config set requirepass "abc\x00def"
        catch {r auth abc} err
        set _ $err
    } {WRONGPASS*}

    test {AUTH succeeds when binary password is correct} {
        r config set requirepass "abc\x00def"
        r auth "abc\x00def"
    } {OK}

    start_server {tags {"primaryauth"}} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set slave [srv 0 client]

        test "primaryauth test with binary password dualchannel = $dualchannel" {
            $master config set requirepass "abc\x00def"
            $master config set dual-channel-replication-enabled $dualchannel
            # Configure the replica with primaryauth
            set loglines [count_log_lines 0]
            $slave config set primaryauth "abc"
            $slave config set dual-channel-replication-enabled $dualchannel
            $slave slaveof $master_host $master_port

            # Verify replica is not able to sync with master
            wait_for_log_messages 0 {"*Unable to AUTH to PRIMARY*"} $loglines 1000 10
            assert_equal {down} [s 0 master_link_status]
            
            # Test replica with the correct primaryauth
            $slave config set primaryauth "abc\x00def"
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Can't turn the instance into a replica"
            }
        }
    }
}
}
