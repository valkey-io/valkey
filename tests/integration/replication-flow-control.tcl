start_server {tags {"repl network external:skip"} overrides {save ""}} {
    set primary [srv 0 client]
    start_server {overrides {save "" io-threads 4 io-threads-always-active yes}} {
        set replica [srv 0 client]
        $replica replicaof [srv -1 host] [srv -1 port]
        wait_for_sync $replica

        test {Primary link stays on the main thread with I/O threads enabled} {
            set reads_before [getInfoProperty [$replica info stats] io_threaded_reads_processed]
            set value [string repeat x 32768]
            for {set i 0} {$i < 128} {incr i} {
                $primary set flow-control:$i $value
            }
            wait_for_ofs_sync $primary $replica
            assert_equal $value [$replica get flow-control:127]
            set reads_after [getInfoProperty [$replica info stats] io_threaded_reads_processed]

            # INFO and GET on the replica can be offloaded, but the replication
            # stream must not contribute to the I/O thread read count.
            assert_lessthan_equal [expr {$reads_after - $reads_before}] 20

            # Verify that I/O thread reads are active for ordinary clients.
            for {set i 0} {$i < 20} {incr i} {
                assert_equal PONG [$replica ping]
            }
            set client_reads [getInfoProperty [$replica info stats] io_threaded_reads_processed]
            assert_morethan $client_reads $reads_after
        }
    }
}
