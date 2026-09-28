set testmodule [file normalize tests/modules/pausetest.so]

# Each test gets its own server pair since the crash kills the server.

# The module command suffix and GET_RESULT key share the same word, but the
# VM_ display name for verbatim is ReplicateVerbatim, so map them explicitly.
foreach {apiname resultkey} {Call call Replicate replicate ReplicateVerbatim verbatim} {
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
                $primary PAUSETEST.TIMER_[string toupper $resultkey] 100
                $primary CLIENT PAUSE 60000 WRITE
                # Without fix: server crashes (assertion in propagateNow)
                # With fix: result=1 (rejected)
                wait_for_condition 50 20 {
                    [$primary PAUSETEST.GET_RESULT $resultkey] ne "NONE"
                } else {
                    fail "Timer result not ready"
                }
                set result [$primary PAUSETEST.GET_RESULT $resultkey]
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
                [$primary PAUSETEST.GET_RESULT call] ne "NONE"
            } else {
                fail "Timer result not ready"
            }
            set result [$primary PAUSETEST.GET_RESULT call]
            assert_equal "OK" $result
        }
    }
}
