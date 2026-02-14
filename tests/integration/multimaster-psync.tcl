start_server {tags {"repl external:skip"} overrides {save {} active-replica yes multi-master yes replica-read-only no}} {
    start_server {overrides {save {} active-replica yes multi-master yes replica-read-only no}} {
        set a [srv -1 client]
        set ah [srv -1 host]
        set ap [srv -1 port]
        set b [srv 0 client]
        set bh [srv 0 host]
        set bp [srv 0 port]

        test {2-node active-active heals after restart and psync} {
            $b replicaof add $ah $ap
            wait_for_condition 100 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "replica link was not established"
            }

            $a select 7
            $b select 7
            $a set x 1
            wait_for_condition 100 100 {
                [$b get x] eq {1}
            } else {
                fail "x did not replicate before restart"
            }

            restart_server 0 true false
            set b [srv 0 client]
            $b config set active-replica yes
            $b config set multi-master yes
            $b config set replica-read-only no
            $b select 7

            $a set y 2
            $b replicaof add $ah $ap
            wait_for_condition 100 100 {
                [s 0 master_link_status] eq {up} &&
                [$b get x] eq {1} &&
                [$b get y] eq {2}
            } else {
                fail "replica did not recover expected dataset after restart"
            }
        }
    }
}
