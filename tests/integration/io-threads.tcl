tags {"io-threads external:skip"} {
    test "Disable io-threads" {
        start_server {overrides {"io-threads" {1} "io-threads-do-reads" {no}}} {
            set master_pid [srv 0 pid]
            set pstree_out [exec ps -T -p $master_pid]
            assert_no_match "*io_thd_*" $pstree_out
            assert_match "*io_threads_maximum_num:1*" [r info server]
            assert_match "*io_threads_active_num:1*" [r info server]

            assert_equal {OK} [r set k v]
            assert_equal {1} [r DBSIZE]
        }
    }

    test "Enable io-threads" {
        start_server {overrides {"io-threads" {4} "io-threads-do-reads" {yes}}} {
            set master_pid [srv 0 pid]
            set pstree_out [exec ps -T -p $master_pid]
            assert_match "*io_thd_1*" $pstree_out
            assert_match "*io_thd_2*" $pstree_out
            assert_match "*io_thd_3*" $pstree_out
            assert_match "*io_threads_maximum_num:4*" [r info server]
            assert_match "*io_threads_active_num:1*" [r info server]

            assert_equal {OK} [r set k v]
            assert_equal {1} [r DBSIZE]
        }
    }
}