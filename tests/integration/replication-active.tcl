start_server {tags {"active-repl external:skip"} overrides {save {}}} {
    start_server {overrides {save {}}} {
        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        set replica [srv 0 client]

        foreach n [list $primary $replica] {
            $n config set active-replica yes
            $n config set multi-master yes
            $n config set replica-read-only no
        }

        $replica replicaof $primary_host $primary_port
        wait_for_condition 100 100 {
            [s 0 master_link_status] eq {up}
        } else {
            fail "active replica link not established"
        }

        test {Active replicas report role metadata} {
            set primary_role [$primary role]
            set replica_role [$replica role]
            assert_equal master [lindex $primary_role 0]
            assert_equal active-replica [lindex $replica_role 0]
        }

        test {Active replicas propagate writes both directions} {
            $primary set ar:key from-primary
            wait_for_condition 100 100 {
                [$replica get ar:key] eq {from-primary}
            } else {
                fail "primary->replica propagation failed"
            }

            $replica set ar:key from-replica
            wait_for_condition 100 100 {
                [$primary get ar:key] eq {from-replica}
            } else {
                fail "replica->primary propagation failed"
            }
        }

        test {Active replicas propagate binary payloads} {
            $primary set ar:bin "\u0000foo"
            wait_for_condition 100 100 {
                [string match *foo* [$replica get ar:bin]]
            } else {
                fail "binary payload did not propagate"
            }
        }
    }
}
