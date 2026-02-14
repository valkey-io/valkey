start_server {tags {"repl external:skip"}} {
start_server {overrides {save {}}} {
start_server {overrides {save {}}} {
    set replica [srv -2 client]
    set replica_host [srv -2 host]
    set p1 [srv -1 client]
    set p1_host [srv -1 host]
    set p1_port [srv -1 port]
    set p2 [srv 0 client]
    set p2_host [srv 0 host]
    set p2_port [srv 0 port]

    test {REPLICAOF ADD configures first upstream and connects} {
        $replica config set active-replica yes
        $replica config set multi-master yes
        $replica config set replica-read-only no
        $replica replicaof add $p1_host $p1_port

        wait_for_condition 100 100 {
            [s -2 master_link_status] eq {up} &&
            [s -2 master_host] eq $p1_host &&
            [s -2 master_port] == $p1_port &&
            [s -2 configured_upstreams] == 1
        } else {
            fail "failed to connect replica to first upstream"
        }
    }

    test {REPLICAOF ADD appends second upstream} {
        $replica replicaof add $p2_host $p2_port
        assert_equal 2 [s -2 configured_upstreams]
    }

    test {ROLE exposes multi-master upstream list} {
        set role_reply [$replica role]
        assert_equal 2 [llength $role_reply]
        assert {[llength [lindex $role_reply 0]] == 5}
        assert {[llength [lindex $role_reply 1]] == 5}
        assert_equal active-replica [lindex [lindex $role_reply 0] 0]
    }

    test {Replication works from first active upstream} {
        $p1 set mm:addremove first-upstream
        wait_for_condition 100 100 {
            [$replica get mm:addremove] eq {first-upstream}
        } else {
            fail "replica did not receive write from first upstream"
        }
    }

    test {REPLICAOF REMOVE current upstream switches to next configured upstream} {
        $replica replicaof remove $p1_host $p1_port
        wait_for_condition 200 100 {
            [s -2 master_host] eq $p2_host &&
            [s -2 master_port] == $p2_port &&
            [s -2 master_link_status] eq {up} &&
            [s -2 configured_upstreams] == 1
        } else {
            fail "replica did not switch to second upstream"
        }

        $p2 set mm:addremove second-upstream
        wait_for_condition 100 100 {
            [$replica get mm:addremove] eq {second-upstream}
        } else {
            fail "replica did not receive write from second upstream"
        }
    }

    test {INFO replication reports multi-master link fields} {
        assert_equal 1 [s -2 connected_masters]
        assert_equal up [s -2 master_global_link_status]
        set m0 [s -2 master_0]
        assert {[string first "active=1" $m0] >= 0}
        assert {[regexp {offset=-?[0-9]+} $m0]}
        assert {[regexp {last_io_seconds_ago=-?[0-9]+} $m0]}
    }

    test {REPLICAOF NO ONE clears configured upstreams} {
        $replica replicaof no one
        assert_equal 0 [s -2 configured_upstreams]
    }
}
}
}
