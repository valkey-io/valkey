start_server {tags {"trusted-clients network external:skip"} overrides {maxclients 10 trusted-maxclients 4 trust-unix-sockets yes}} {
    if {$::tls} {
        set expected_code "*I/O error*"
    } else {
        set expected_code "*ERR max*reached*"
    }

    # Use harness-resolved path: works for Make (src/) and CMake (build-release/bin/)
    set cli_path $::VALKEY_CLI_BIN

    test {trusted-maxclients reserves capacity out of maxclients, not beyond it} {
        # maxclients 10, trusted-maxclients 4 => only 6 slots for normal clients.
        set c 0
        catch {
            while {$c < 20} {
                incr c
                set rd [valkey_deferring_client]
                $rd ping
                $rd read
                after 100
            }
        } e
        assert {$c > 4 && $c <= 6}
        set e
    } $expected_code

    test {trusted (unix socket) clients can still connect once normal pool is full} {
        # Normal pool (6 slots) is exhausted from the previous test's leaked
        # deferring clients. A trusted connection over the Unix socket must
        # still be admitted, up to trusted-maxclients.
        set sock [srv 0 "unixsocket"]
        set out [exec $cli_path -s $sock ping]
        assert_equal {PONG} $out
    }

    test {5th trusted client is rejected when trusted-maxclients is 4 (idle server)} {
        # The harness connection r is TCP from 127.0.0.1 and NOT trusted (only
        # trust-unix-sockets is on, no trusted-sources), so it can observe the
        # server without consuming a trusted slot. We hold trusted clients open
        # with BLPOP (blocks the client, not the server event loop) and confirm
        # exactly trusted-maxclients are admitted and the next is rejected.
        set sock [srv 0 "unixsocket"]
        set rejected_before [getInfoProperty [r info stats] rejected_trusted_connections]
        set held {}
        for {set i 0} {$i < 4} {incr i} {
            lappend held [open "|$cli_path -s $sock blpop trusted-test-nokey 5" "r"]
            after 100
        }
        wait_for_condition 50 100 {
            [s trusted_connections] == 4
        } else {
            fail "expected 4 trusted connections, got [s trusted_connections]"
        }
        # A 5th trusted connection must be rejected; valkey-cli exits non-zero
        # when the server refuses it, so we assert on the server-side counter.
        catch {exec $cli_path -s $sock ping}
        wait_for_condition 50 100 {
            [getInfoProperty [r info stats] rejected_trusted_connections] > $rejected_before
        } else {
            fail "5th trusted client was not rejected"
        }
        foreach fd $held { catch {close $fd} }
    }

    test {INFO reports trusted_connections and trusted_maxclients} {
        set info [r info clients]
        assert_match "*trusted_maxclients:4*" $info
        assert_match "*trusted_connections:*" $info
    }
}

start_server {tags {"trusted-clients network external:skip"}} {
    test {trusted-maxclients must be less than maxclients} {
        r config set maxclients 100
        catch {r config set trusted-maxclients 100} e
        assert_match "*must be less than maxclients*" $e
        catch {r config set trusted-maxclients 150} e
        assert_match "*must be less than maxclients*" $e
        r config set trusted-maxclients 50
    } {OK}

    test {lowering maxclients below trusted-maxclients is rejected} {
        set orig_maxclients [lindex [r config get maxclients] 1]
        r config set trusted-maxclients 50
        catch {r config set maxclients 40} e
        assert_match "*must be less than maxclients*" $e
        # restore
        r config set maxclients $orig_maxclients
    }

    test {trusted-sources rejects invalid entries} {
        catch {r config set trusted-sources "not-an-ip"} e
        assert_match "*Invalid IP address*" $e

        catch {r config set trusted-sources "10.0.0.0/40"} e
        assert_match "*Invalid IPv4 CIDR prefix*" $e

        catch {r config set trusted-sources "::1/200"} e
        assert_match "*Invalid CIDR prefix*" $e
    }

    test {trusted-sources rejects overflowing / out-of-range CIDR prefixes} {
        # Values that overflow long, or exceed the widest valid prefix, must
        # be rejected before the cast to int rather than wrapping past the
        # per-family range checks.
        catch {r config set trusted-sources "10.0.0.0/99999999999999999999"} e
        assert_match "*Invalid CIDR prefix*" $e

        catch {r config set trusted-sources "::1/99999999999999999999"} e
        assert_match "*Invalid CIDR prefix*" $e

        catch {r config set trusted-sources "10.0.0.0/4294967329"} e
        assert_match "*Invalid CIDR prefix*" $e
    }

    test {trusted-sources accepts valid IPv4, IPv6, and CIDR entries} {
        r config set trusted-sources "127.0.0.1 10.0.0.0/8 ::1/128"
    } {OK}

    test {trusted-sources can be cleared} {
        r config set trusted-sources ""
    } {OK}
}

start_server {tags {"trusted-clients network external:skip"} overrides {maxclients 10}} {
    test {default trusted-maxclients (0) leaves maxclients fully usable} {
        # Regression guard: a non-zero default here would silently reserve
        # slots out of small maxclients values and lock everyone out.
        assert_equal {0} [lindex [r config get trusted-maxclients] 1]
        assert_equal {PONG} [r ping]
    }

    test {an over-large trusted-maxclients still leaves one normal slot} {
        # Even if misconfigured close to maxclients, normal clients keep at
        # least one slot rather than being fully starved.
        r config set trusted-maxclients 9
        assert_equal {PONG} [r ping]
        r config set trusted-maxclients 0
    }
}
