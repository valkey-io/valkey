set testmodule [file normalize tests/modules/pausetest.so]

# Each test gets its own server pair since the crash kills the server.

# Map each API to its command suffix (differs from the VM_ name for verbatim)
# and expected result: VM_Call rejects with a NULL reply, VM_Replicate* error.
foreach {apiname cmdsuffix expected} {
    Call CALL NULL
    Replicate REPLICATE ERROR
    ReplicateVerbatim VERBATIM ERROR
} {
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

            test "VM_$apiname write returns error during CLIENT PAUSE WRITE with replica" {
                # TIMER_* is not a write command, so it can be scheduled after the
                # pause; the write runs inside the timer callback while paused.
                $primary CLIENT PAUSE 60000 WRITE
                $primary PAUSETEST.TIMER_$cmdsuffix 1
                # Without fix: server crashes (assertion in propagateNow)
                # With fix: the write is rejected (result != OK)
                wait_for_condition 50 20 {
                    [$primary PAUSETEST.GET_RESULT] ne "NONE"
                } else {
                    fail "Timer result not ready"
                }
                set result [$primary PAUSETEST.GET_RESULT]
                $primary CLIENT UNPAUSE
                assert_equal $expected $result
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
            $primary PAUSETEST.TIMER_CALL 1
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
