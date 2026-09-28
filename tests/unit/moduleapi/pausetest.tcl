set testmodule [file normalize tests/modules/pausetest.so]

# Each test gets its own server pair since the crash kills the server.

# The command suffix and VM_ display name differ for verbatim
# (VERBATIM vs ReplicateVerbatim), so map them explicitly.
foreach {apiname cmdsuffix} {Call CALL Replicate REPLICATE ReplicateVerbatim VERBATIM} {
    start_server {tags {"modules needs:repl"}} {
        r module load $testmodule
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        start_server {} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 100 {
                [string match {*master_link_status:up*} [$replica info replication]]
            } else {
                fail "Replica did not connect"
            }

            test "VM_$apiname write command returns error during CLIENT PAUSE WRITE with replica" {
                $primary PAUSETEST.TIMER_$cmdsuffix 100
                $primary CLIENT PAUSE 60000 WRITE
                # Without fix: server crashes (assertion in propagateNow)
                # With fix: result=ERROR (rejected)
                wait_for_condition 50 20 {
                    [$primary PAUSETEST.GET_RESULT] ne "NONE"
                } else {
                    fail "Timer result not ready"
                }
                set result [$primary PAUSETEST.GET_RESULT]
                $primary CLIENT UNPAUSE
                assert_equal "ERROR" $result
            }
        }
    }
}

start_server {tags {"modules needs:repl"}} {
    r module load $testmodule
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    start_server {} {
        set replica [srv 0 client]
        $replica replicaof $primary_host $primary_port
        wait_for_condition 50 100 {
            [string match {*master_link_status:up*} [$replica info replication]]
        } else {
            fail "Replica did not connect"
        }

        test {Module timer VM_Call succeeds when not paused} {
            $primary PAUSETEST.TIMER_CALL 100
            wait_for_condition 50 20 {
                [$primary PAUSETEST.GET_RESULT] ne "NONE"
            } else {
                fail "Timer result not ready"
            }
            set result [$primary PAUSETEST.GET_RESULT]
            assert_equal "OK" $result
        }
    }
}
