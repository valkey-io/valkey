# A disk-loading dual-channel replica must survive the primary dropping the
# main channel while the replica loads the RDB. The load paths a cancel can
# take are covered by the RdbLoadCancelTest unit tests.
start_server {tags {"dual-channel-replication external:skip"}} {
    set primary [srv 0 client]
    $primary config set dual-channel-replication-enabled yes
    $primary config set repl-diskless-sync yes
    $primary config set repl-diskless-sync-delay 0
    $primary debug populate 20000 key 100
    start_server {} {
        set replica [srv 0 client]
        set replica_pid [srv 0 pid]
        $replica config set dual-channel-replication-enabled yes
        $replica config set repl-diskless-load disabled
        $replica config set loading-process-events-interval-bytes 1024
        # 20000 keys * 500 us: the load takes about 10 s.
        $replica config set key-load-delay 500
        test "Disk-loading replica survives a main channel drop during the RDB load" {
            set loglines [count_log_lines 0]
            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_log_messages 0 {"*Loading RDB produced by*"} $loglines 1000 10
            $primary client kill type replica
            wait_for_log_messages 0 {"*RDB load aborted because the replication link was lost*"} $loglines 1000 10
            assert {[process_is_alive $replica_pid]}

            # The aborted load is discarded and the next full sync completes.
            $replica config set key-load-delay 0
            wait_for_condition 500 20 {
                [s 0 master_link_status] eq {up} && [$replica dbsize] == [$primary dbsize]
            } else {
                fail "Replica did not resync after the aborted load"
            }
        }
    }
}
