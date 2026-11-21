# With io-threads, an on-empty-db replica must get no stream before its first ACK.
foreach {forkless} {yes no} {
    set desc [expr {$forkless eq "yes" ? "forkless" : "fork-based"}]
    start_server [list tags {"repl external:skip tls:skip needs:debug"} overrides [list save "" forkless-infrastructure-enabled yes forkless-policy [expr {$forkless eq "yes" ? "prefer-forkless" : "require-fork"}] repl-diskless-sync yes repl-diskless-sync-delay 0 io-threads 4]] {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        $primary debug populate 50000
        start_server {overrides {save "" repl-diskless-load on-empty-db}} {
            set replica [srv 0 client]

            test "No replication stream before the first ACK of an on-empty-db replica ($desc sync)" {
                # INCR on random keys: every lost command changes the dataset.
                set bench [exec $::VALKEY_BENCHMARK_BIN -h $primary_host -p $primary_port -r 50000 \
                    -n 100000000 -P 8 -c 2 -q INCR cnt:__rand_int__ > /dev/null 2>@1 &]
                after 300
                $primary config set rdb-key-save-delay 1
                $replica replicaof $primary_host $primary_port
                wait_for_condition 600 100 {
                    [s 0 master_link_status] eq {up}
                } else {
                    fail "replica did not sync"
                }
                after 1000
                catch {exec /bin/kill -9 $bench}
                wait_for_ofs_sync $primary $replica
                assert_equal [$primary debug digest] [$replica debug digest]
            }
        }
    }
}
