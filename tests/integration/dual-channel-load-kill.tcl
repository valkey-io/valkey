# A dual-channel replica must survive the primary dropping the main channel
# while the replica loads the RDB and its local replication buffer is not full.
# readIntoReplDataBlock() sees the EOF and cancels the load with
# rioCloseASAP(server.loading_rio). With repl-diskless-load disabled the
# loading rio is the downloaded RDB file, so the load must end as a failed
# load, not as a truncated RDB file (rdbReportError() would run the RDB checker
# and exit the replica). The stream dataset makes the cancelled read fail
# inside rdbLoadObject(), which reports the error itself, instead of at the
# loader's eoferr label.
foreach {mode dataset} {disabled strings on-empty-db strings disabled streams} {
start_server {tags {"dual-channel-replication external:skip"}} {
    set primary [srv 0 client]
    $primary config set dual-channel-replication-enabled yes
    $primary config set repl-diskless-sync yes
    $primary config set repl-diskless-sync-delay 0
    if {$dataset eq "strings"} {
        $primary debug populate 20000 key 100
        # 20000 keys * 500 us: the load takes about 10 s.
        set key_load_delay 500
    } else {
        # 2000 streams of 20 entries, each bigger than the 1024 byte read chunk.
        $primary eval {
            for i = 1, 2000 do
                for j = 1, 20 do
                    server.call('xadd', 'stream:' .. i, '*', 'f', string.rep('x', 200))
                end
            end
        } 0
        # 2000 keys * 5000 us: the load takes about 10 s.
        set key_load_delay 5000
    }
    start_server {} {
        set replica [srv 0 client]
        set replica_pid [srv 0 pid]
        $replica config set dual-channel-replication-enabled yes
        $replica config set repl-diskless-load $mode
        $replica config set loading-process-events-interval-bytes 1024
        $replica config set key-load-delay $key_load_delay
        test "Replica survives a main channel drop during the RDB load, buffer not full ($mode, $dataset)" {
            set loglines [count_log_lines 0]
            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_log_messages 0 {"*Loading RDB produced by*"} $loglines 1000 10
            after 200
            $primary client kill type replica
            # The replica notices the drop through its main-channel read handler.
            wait_for_log_messages 0 {"*Provisional primary closed connection*"} $loglines 200 10
            after 2000
            if {![process_is_alive $replica_pid]} {
                fail "Replica exited after the main channel was dropped during the RDB load (repl-diskless-load $mode, $dataset)"
            }
            # The aborted load is discarded and the next full sync completes.
            $replica config set key-load-delay 0
            wait_for_condition 500 20 {
                [s 0 master_link_status] eq {up} && [$replica dbsize] == [$primary dbsize]
            } else {
                fail "Replica did not resync after the aborted load"
            }
            assert_equal [$primary debug digest] [$replica debug digest]
        }
    }
}
}
