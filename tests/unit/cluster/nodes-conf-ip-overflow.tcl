tags {external:skip cluster} {
    test {nodes.conf address longer than NET_IP_STR_LEN is rejected as corrupt} {
        set dir [file normalize [tmpdir nodes-conf-ip-overflow]]
        set conf_path [file join $dir nodes.conf]
        set log_path [file join $dir valkey.log]
        write_file $conf_path "[string repeat a 40] [string repeat A 80]:6379@16379 myself,master - 0 0 0 connected\nvars currentEpoch 0 lastVoteEpoch 0"

        set pid [exec $::VALKEY_SERVER_BIN --port 0 --bind 127.0.0.1 --cluster-enabled yes \
                     --dir $dir --cluster-config-file $conf_path --logfile $log_path \
                     --use-exit-on-panic yes --crash-log-enabled no &]

        wait_for_condition 100 100 {
            ![process_is_alive $pid]
        } else {
            catch {exec kill -9 $pid}
            fail "server did not exit"
        }

        set log [read_file $log_path]
        assert_no_match {*crashed by signal*} $log
        assert_no_match {*Node configuration loaded*} $log
        assert_match {*corrupted cluster config file*} $log
    }
}
