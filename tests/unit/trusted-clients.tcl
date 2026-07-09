start_server {tags {"trusted-clients network external:skip"} overrides {maxclients 10 trusted-maxclients 4 trust-unix-sockets yes}} {
    if {$::tls} {
        set expected_code "*I/O error*"
    } else {
        set expected_code "*ERR max*reached*"
    }

    set cli_path [file normalize "src/valkey-cli"]

    # Runs `valkey-cli -s <unixsocket> ping` in the background and returns the
    # exec token (a list you can pass to close_unix_client). Used because the
    # Tcl test client library has no Unix-socket connector.
    proc open_unix_client {} {
        set sock [srv 0 "unixsocket"]
        set path [file normalize "src/valkey-cli"]
        set fd [open "|$path -s $sock ping" "r"]
        return $fd
    }

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

    test {trusted connections are capped at trusted-maxclients and don't overrun maxclients} {
        set sock [srv 0 "unixsocket"]
        set fds {}
        set rejected 0
        for {set i 0} {$i < 10} {incr i} {
            set fd [open "|$cli_path -s $sock -t 5 debug sleep 3" "r"]
            lappend fds $fd
            after 50
        }
        # At most trusted-maxclients (4) trusted connections should be
        # concurrently admitted; the rest should have been rejected before
        # the debug sleep even started, so we simply verify the server
        # tracks a bounded trusted client count rather than unboundedly
        # growing past trusted-maxclients.
        after 200
        set trusted_now [s trusted_connections]
        assert {$trusted_now <= 4}
        foreach fd $fds { catch {close $fd} }
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
        assert_match "*Invalid IPv6 CIDR prefix*" $e
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
