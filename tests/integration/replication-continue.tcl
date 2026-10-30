# The fake primary only speaks plain TCP.
if {!$::tls} {
    tags {"repl external:skip"} {
        test "Replica survives +CONTINUE reply to PSYNC without a cached primary" {
            set port [find_available_port $::baseport $::portcount]
            set pid [exec [info nameofexecutable] tests/helpers/fake_primary.tcl \
                         $port /dev/null 0 "" +CONTINUE &]
            wait_for_condition 50 50 {
                [ping_server 127.0.0.1 $port]
            } else {
                fail "Failed to start fake primary"
            }

            # Starting as a replica with no RDB leaves no cached primary, so the
            # replica sends PSYNC ? -1. REPLICAOF on a primary would cache itself.
            start_server [list overrides [list replicaof "127.0.0.1 $port"]] {
                set replica [srv 0 client]

                # The fake primary exits once the replica drops the link.
                wait_for_condition 50 100 {
                    [catch {exec kill -0 $pid}]
                } else {
                    catch {exec kill $pid}
                    fail "Replica did not drop the link after +CONTINUE"
                }
                assert_equal PONG [$replica ping]
            }
        }
    }
}
