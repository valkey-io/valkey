start_server {tags {"repl external:skip"} overrides {save {} active-replica yes multi-master yes replica-read-only no client-output-buffer-limit {replica 100mb 100mb 999999}}} {
    start_server {overrides {save {} active-replica yes multi-master yes replica-read-only no client-output-buffer-limit {replica 100mb 100mb 999999}}} {
        set a [srv -1 client]
        set ah [srv -1 host]
        set ap [srv -1 port]
        set b [srv 0 client]
        set bh [srv 0 host]
        set bp [srv 0 port]

        $a config set repl-backlog-size 100mb
        $b config set repl-backlog-size 100mb

        test {Active-active link bootstrap for psync test} {
            $b replicaof add $ah $ap
            wait_for_condition 100 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "replica link was not established"
            }
        }

        test {Replication stays consistent during repeated reconnects} {
            for {set i 0} {$i < 80} {incr i} {
                $a incr rpm:counter
                if {($i % 7) == 0} {
                    catch {$b client kill type master}
                }
                after 10
            }

            wait_for_condition 150 100 {
                [$a debug digest] eq [$b debug digest]
            } else {
                fail "master and replica diverged after reconnect churn"
            }
        }
    }
}
