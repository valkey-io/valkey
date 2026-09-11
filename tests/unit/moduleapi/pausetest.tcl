set testmodule [file normalize tests/modules/pausetest.so]

# Each test gets its own server pair since the crash kills the server.

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

        test {VM_Call write command returns error during CLIENT PAUSE WRITE with replica} {
            $primary PAUSETEST.TIMER_CALL 200
            $primary CLIENT PAUSE 60000 WRITE
            after 500
            # Without fix: server crashes (assertion in propagateNow)
            # With fix: result=1 (rejected)
            set result [$primary PAUSETEST.GET_RESULT call]
            $primary CLIENT UNPAUSE
            assert_equal 1 $result
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

        test {VM_Replicate returns error during CLIENT PAUSE WRITE with replica} {
            $primary PAUSETEST.TIMER_REPLICATE 200
            $primary CLIENT PAUSE 60000 WRITE
            after 500
            set result [$primary PAUSETEST.GET_RESULT replicate]
            $primary CLIENT UNPAUSE
            assert_equal 1 $result
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

        test {VM_ReplicateVerbatim returns error during CLIENT PAUSE WRITE with replica} {
            $primary PAUSETEST.TIMER_VERBATIM 200
            $primary CLIENT PAUSE 60000 WRITE
            after 500
            set result [$primary PAUSETEST.GET_RESULT verbatim]
            $primary CLIENT UNPAUSE
            assert_equal 1 $result
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
            after 300
            set result [$primary PAUSETEST.GET_RESULT call]
            assert_equal 0 $result
        }
    }
}
