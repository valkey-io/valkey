proc get_io_threads_active_num {client} {
    set server_info [$client info server]
    regexp {io_threads_active_num:([0-9]+)} $server_info ->\
        io_threads_active_num
    return $io_threads_active_num
}

proc start_benchmark {thread_num client_num} {
    set bench_pid [
        exec src/valkey-benchmark -s [srv unixsocket] -t set \
        -r 5000000 -n 10000000 --threads $thread_num -c $client_num \
        > /dev/null &
    ]
    return $bench_pid
}

tags {"io-threads external:skip"} {
    test "Disable io-threads" {
        start_server {overrides {"io-threads" {1} "io-threads-do-reads" {no}}} {
            set master_pid [srv 0 pid]
            set pstree_out [exec ps -T -p $master_pid]
            assert_no_match "*io_thd_*" $pstree_out
            assert_match "*io_threads_active:0*" [r info server]
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
            set server_info [r info server]
            regexp {io_threads_active:([0-9]+)} $server_info -> \
                io_threads_active
            regexp {io_threads_maximum_num:([0-9]+)} $server_info -> \
                io_threads_maximum_num
            regexp {io_threads_active_num:([0-9]+)} $server_info -> \
                io_threads_active_num
            assert {$io_threads_active == 0}
            assert {$io_threads_maximum_num == 4}
            assert {$io_threads_active_num == 1}

            assert_equal {OK} [r set k v]
            assert_equal {1} [r DBSIZE]
        }
    }

    test "Test scale up and scale back down io_threads_active_num." {
        start_server {overrides {"io-threads" {4} "io-threads-do-reads" {yes}}} {
            set client [valkey [srv host] [srv port] 0 $::tls]
            # When the server has just started, no requests from client.
            # The active threads number should be 1.
            assert {[get_io_threads_active_num $client] == 1}

            # Scale up io_threads_active_num by sending large number requests to server.
            set bench_pid [start_benchmark 10 50]
            wait_for_condition 1000 500 {
                [$client DBSIZE] > 10000
            } else {
                fail "Benchmark failed to write data to server."
            }
            assert {[get_io_threads_active_num $client] == 4}
            exec kill -9 $bench_pid

            # Send flushdb request to server, the server.clients_pending_write should be 1.
            # But dut to the exponential moving average, the clients_pending_write_avg_num
            # didn't change too much.
            # Server still keep 4 io-threads to handle the requests.
            $client FLUSHDB
            assert {[$client DBSIZE] == 0}
            assert {[get_io_threads_active_num $client] == 4}

            # Scale down io_threads_active_num by sending small number requests to server.
            set bench_pid [start_benchmark 1 1]
            wait_for_condition 1000 500 {
                [$client DBSIZE] > 1000
            } else {
                fail "Benchmark failed to write data to server."
            }
            assert {[get_io_threads_active_num $client] == 1}
            exec kill -9 $bench_pid
        }
    }
}
