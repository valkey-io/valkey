start_server {tags {"repl external:skip"}} {
start_server {overrides {save {}}} {
start_server {overrides {save {}}} {
    set nodeA [srv -2 client]
    set nodeA_host [srv -2 host]
    set nodeA_port [srv -2 port]
    set nodeB [srv -1 client]
    set nodeB_host [srv -1 host]
    set nodeB_port [srv -1 port]
    set nodeC [srv 0 client]
    set nodeC_host [srv 0 host]
    set nodeC_port [srv 0 port]

    foreach n [list $nodeA $nodeB $nodeC] {
        $n config set active-replica yes
        $n config set multi-master yes
        $n config set replica-read-only no
    }

    test {3-node chain converges with active-replica and no-forward enabled} {
        $nodeB replicaof add $nodeA_host $nodeA_port
        $nodeC replicaof $nodeB_host $nodeB_port
        wait_for_condition 100 100 {
            [s -1 master_link_status] eq {up} &&
            [s 0 master_link_status] eq {up}
        } else {
            fail "chain links were not established"
        }

        $nodeB config set multi-master-no-forward yes
        $nodeB set mm:nf v1
        wait_for_condition 100 100 {
            [$nodeA get mm:nf] eq {v1}
        } else {
            fail "upstream did not receive replayed update"
        }

        wait_for_condition 100 100 {
            [$nodeC get mm:nf] eq {v1}
        } else {
            fail "downstream did not converge in chain"
        }
        assert {[string first "state=" [s -1 master_0]] >= 0}

        $nodeB config set multi-master-no-forward no
        $nodeB set mm:nf v2
        wait_for_condition 150 100 {
            [$nodeA get mm:nf] eq {v2} &&
            [$nodeB get mm:nf] eq {v2} &&
            [$nodeC get mm:nf] eq {v2}
        } else {
            fail "downstream did not receive replay after re-enabling forwarding"
        }
    }

    test {3-node failover between configured upstreams} {
        foreach n [list $nodeA $nodeB $nodeC] {
            $n replicaof no one
            $n flushall
            $n config set active-replica yes
            $n config set multi-master yes
            $n config set replica-read-only no
        }

        $nodeB replicaof add $nodeA_host $nodeA_port
        $nodeB replicaof add $nodeC_host $nodeC_port
        wait_for_condition 100 100 {
            [s -1 master_link_status] eq {up} &&
            [s -1 master_host] eq $nodeA_host &&
            [s -1 configured_upstreams] == 2
        } else {
            fail "nodeB did not establish first upstream"
        }

        $nodeB replicaof remove $nodeA_host $nodeA_port
        wait_for_condition 150 100 {
            [s -1 master_link_status] eq {up} &&
            [s -1 master_host] eq $nodeC_host &&
            [s -1 configured_upstreams] == 1
        } else {
            fail "nodeB did not fail over to second upstream"
        }

        $nodeC set mm:failover ok
        wait_for_condition 100 100 {
            [$nodeB get mm:failover] eq {ok}
        } else {
            fail "nodeB did not replicate from failed-over upstream"
        }
    }

}
}
}
