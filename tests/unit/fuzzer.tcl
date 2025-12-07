tags {"slow"} {
    run_solo {fuzzer} {
        start_server {} {
            test {FUZZ stresser with valkey-benchmark} {
                assert_equal [r ping] {PONG}
                set err [catch {exec src/valkey-benchmark  -p [srv 0 port] -c 20 -n 100000 --fuzz --fuzz-loglevel info} output]
                if {$err && $::verbose} {
                    # For now, if the server is still responsive, we don't consider the test a failure even if the fuzzer failed.
                    puts $output
                }
                # Verify server is still responsive after the fuzzer run
                # Create a new client connection in case the previous one was closed by the fuzzer.
                set rr [valkey_client]
                assert_equal [$rr ping] {PONG}
            }
        }
    }
}
