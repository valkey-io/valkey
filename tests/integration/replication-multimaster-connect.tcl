start_server {tags {"multi-master external:skip"} overrides {save {} active-replica yes multi-master yes replica-read-only no}} {
start_server {overrides {save {} active-replica yes multi-master yes replica-read-only no}} {
start_server {overrides {save {} active-replica yes multi-master yes replica-read-only no}} {
start_server {overrides {save {} active-replica yes multi-master yes replica-read-only no}} {
    set replica [srv -3 client]
    set p1 [srv -2 client]
    set p1_host [srv -2 host]
    set p1_port [srv -2 port]
    set p2 [srv -1 client]
    set p2_host [srv -1 host]
    set p2_port [srv -1 port]
    set p3 [srv 0 client]
    set p3_host [srv 0 host]
    set p3_port [srv 0 port]

    test {Connect to first upstream and replicate} {
        $replica replicaof add $p1_host $p1_port
        wait_for_condition 100 100 {
            [s -3 master_link_status] eq {up} &&
            [s -3 master_host] eq $p1_host
        } else {
            fail "replica did not connect to upstream #1"
        }

        $p1 set mmc:key1 v1
        wait_for_condition 100 100 {
            [$replica get mmc:key1] eq {v1}
        } else {
            fail "replica did not receive data from upstream #1"
        }
    }

    test {Connect to additional upstreams} {
        $replica replicaof add $p2_host $p2_port
        $replica replicaof add $p3_host $p3_port
        assert_equal 3 [s -3 configured_upstreams]
    }

    test {Per-peer PSYNC reconnect failover stress in concurrent mode} {
        set observed_ports {}
        for {set i 0} {$i < 18} {incr i} {
            catch {$replica client kill type master}
            wait_for_condition 150 100 {
                [s -3 master_link_status] eq {up}
            } else {
                fail "replica did not reconnect during failover stress loop #$i"
            }

            set active_port [s -3 master_port]
            lappend observed_ports $active_port

            set writer ""
            if {$active_port == $p1_port} {
                set writer $p1
            } elseif {$active_port == $p2_port} {
                set writer $p2
            } elseif {$active_port == $p3_port} {
                set writer $p3
            } else {
                fail "unexpected active upstream port $active_port"
            }

            set k "mmc:stress:$i"
            set v "v$i"
            $writer set $k $v
            wait_for_condition 100 100 {
                [$replica get $k] eq $v
            } else {
                fail "replica did not apply stress key $k from upstream port $active_port"
            }
        }

        set uniq_ports [lsort -unique $observed_ports]
        assert {[llength $uniq_ports] >= 2}
    }

    test {Switch to second upstream and replicate} {
        $replica replicaof remove $p1_host $p1_port
        wait_for_condition 150 100 {
            [s -3 master_host] eq $p2_host &&
            [s -3 master_link_status] eq {up}
        } else {
            fail "replica did not fail over to upstream #2"
        }

        $p2 set mmc:key2 v2
        wait_for_condition 100 100 {
            [$replica get mmc:key2] eq {v2}
        } else {
            fail "replica did not receive data from upstream #2"
        }
    }

    test {Switch to third upstream and replicate} {
        $replica replicaof remove $p2_host $p2_port
        wait_for_condition 150 100 {
            [s -3 master_host] eq $p3_host &&
            [s -3 master_link_status] eq {up}
        } else {
            fail "replica did not fail over to upstream #3"
        }

        $p3 set mmc:key3 v3
        wait_for_condition 100 100 {
            [$replica get mmc:key3] eq {v3}
        } else {
            fail "replica did not receive data from upstream #3"
        }
    }
}}}}
