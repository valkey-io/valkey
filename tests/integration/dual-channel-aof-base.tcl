# A disk-load dual-channel full sync buffers the primary's stream on the main
# channel while the RDB loads, then applies that buffer with AOF still off.
# The AOF that is turned back on afterwards must contain those writes.
foreach preamble {yes no} {
start_server {tags {"dual-channel-replication repl external:skip"}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    $primary config set repl-diskless-sync yes
    $primary config set repl-diskless-sync-delay 0
    $primary config set dual-channel-replication-enabled yes
    $primary debug populate 1000 key 100

    start_server [list overrides [list appendonly yes aof-use-rdb-preamble $preamble save ""]] {
        set replica [srv 0 client]
        $replica config set dual-channel-replication-enabled yes
        $replica config set repl-diskless-load disabled
        $replica config set loading-process-events-interval-bytes 1024
        # 10 ms per key: the RDB load takes about 10 s, so the writes below
        # land in the replica's local replication buffer.
        $replica config set key-load-delay 10000

        test "Dual-channel disk-load sync keeps buffered writes in the AOF (aof-use-rdb-preamble $preamble)" {
            set loglines [count_log_lines 0]
            $replica replicaof $primary_host $primary_port
            wait_for_condition 500 20 {[s 0 loading] eq 1} else {
                fail "replica did not start loading the RDB"
            }
            for {set i 0} {$i < 200} {incr i} {
                $primary set buffered:$i v$i
            }
            wait_for_condition 500 20 {[s 0 replicas_repl_buffer_size] > 0} else {
                fail "replica did not buffer the stream"
            }
            assert_equal 1 [s 0 loading]
            $replica config set key-load-delay 0

            wait_for_log_messages 0 {"*Successfully streamed replication data into memory*"} $loglines 500 20
            wait_for_condition 500 20 {[s 0 master_link_status] eq "up"} else {
                fail "replica link did not come up"
            }
            wait_for_ofs_sync $primary $replica
            wait_for_condition 500 20 {
                [s 0 aof_rewrite_in_progress] == 0 &&
                [s 0 aof_rewrite_scheduled] == 0
            } else {
                fail "AOF rewrite did not finish"
            }
            assert_equal 1200 [$replica dbsize]
            assert_equal [$primary debug digest] [$replica debug digest]
            assert_equal 0 [count_message_lines [srv 0 stdout] "Reused RDB file from primary sync as AOF base file"]

            # Reload the dataset from the AOF alone.
            set digest [$replica debug digest]
            $replica debug loadaof
            assert_equal 1200 [$replica dbsize]
            assert_equal 1 [$replica exists buffered:0]
            assert_equal $digest [$replica debug digest]
        }
    }
}
}
