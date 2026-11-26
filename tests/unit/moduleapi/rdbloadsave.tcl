set testmodule [file normalize tests/modules/rdbloadsave.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module rdbloadsave sanity" {
        r test.sanity

        # Try to load non-existing file
        assert_error {*No such file or directory*} {r test.rdbload sanity.rdb}

        r set x 1
        assert_equal OK [r test.rdbsave sanity.rdb]

        r flushdb
        assert_equal OK [r test.rdbload sanity.rdb]
        assert_equal 1 [r get x]
    }

    test "Module rdbloadsave test with pipelining" {
        r config set save ""
        r config set loading-process-events-interval-bytes 1024
        r config set key-load-delay 50
        r flushdb

        populate 3000 a 1024
        r set x 111
        assert_equal [r dbsize] 3001

        assert_equal OK [r test.rdbsave blabla.rdb]
        r flushdb
        assert_equal [r dbsize] 0

        # Send commands with pipeline. First command will call RM_RdbLoad() in
        # the command callback. While loading RDB, the server can go to networking to
        # reply -LOADING. By sending commands in pipeline, we verify it doesn't
        # cause a problem.
        # e.g. the server won't try to process next message of the current client
        # while it is in the command callback for that client   .
        set rd1 [valkey_deferring_client]
        $rd1 test.rdbload blabla.rdb

        wait_for_condition 50 100 {
            [s loading] eq 1
        } else {
            fail "Redis did not start loading or loaded RDB too fast"
        }

        $rd1 get x
        $rd1 dbsize

        assert_equal OK [$rd1 read]
        assert_equal 111 [$rd1 read]
        assert_equal 3001 [$rd1 read]
        r flushdb
        r config set key-load-delay 0
    }

    test "Module rdbloadsave with aof" {
        r config set save ""

        # Enable the AOF
        r config set appendonly yes
        r config set auto-aof-rewrite-percentage 0 ; # Disable auto-rewrite.
        waitForBgrewriteaof r

        r set k v1
        assert_equal OK [r test.rdbsave aoftest.rdb]

        r set k v2
        r config set rdb-key-save-delay 10000000
        r bgrewriteaof

        # RM_RdbLoad() should kill aof fork
        assert_equal OK [r test.rdbload aoftest.rdb]

        wait_for_condition 50 100 {
            [string match {*Killing*AOF*child*} [exec tail -20 < [srv 0 stdout]]]
        } else {
            fail "Can't find 'Killing AOF child' in recent log lines"
        }

        # Verify that AOF is active.
        assert_equal 1 [s aof_enabled]

        # Verify the value in the loaded rdb
        assert_equal v1 [r get k]

        r flushdb
        r config set rdb-key-save-delay 0
        r config set appendonly no
    }

    test "Module rdbloadsave with bgsave" {
        r flushdb
        r config set save ""

        r set k v1
        assert_equal OK [r test.rdbsave bgsave.rdb]

        r set k v2
        r config set rdb-key-save-delay 500000
        r bgsave

        # RM_RdbLoad() should kill RDB fork
        assert_equal OK [r test.rdbload bgsave.rdb]

        wait_for_condition 10 1000 {
            [string match {*Background*saving*terminated*} [exec tail -20 < [srv 0 stdout]]]
        } else {
            fail "Can't find 'Background saving terminated' in recent log lines"
        }

        assert_equal v1 [r get k]
        r flushall
        waitForBgsave r
        r config set rdb-key-save-delay 0
    }

    test "Module rdbloadsave calls rdbsave in a module fork" {
        r flushdb
        r config set save ""
        r config set rdb-key-save-delay 500000

        r set k v1

        # Module will call RM_Fork() before calling RM_RdbSave()
        assert_equal OK [r test.rdbsave_fork rdbfork.rdb]
        assert_equal [s module_fork_in_progress] 1

        wait_for_condition 10 1000 {
            [status r module_fork_in_progress] == "0"
        } else {
            fail "Module fork didn't finish"
        }

        r set k v2
        assert_equal OK [r test.rdbload rdbfork.rdb]
        assert_equal v1 [r get k]

        r config set rdb-key-save-delay 0
    }

    test "Module rdbsave cleans up after an LZ4 write error" {
        set dir [lindex [r config get dir] 1]
        set failed_rdb [file join $dir failed-lz4-save.rdb]
        r config set rdbcompression lz4
        r flushdb
        r set lz4:write-error [read_binary_file_prefix /dev/urandom [expr {1024 * 1024}]]

        with_cleanup {
            file delete -force $failed_rdb
            exec mkfifo $failed_rdb
            # Closing the FIFO reader after one byte forces a write error while
            # rdbSaveRio is emitting the compressed RDB.
            exec dd if=$failed_rdb of=/dev/null bs=1 count=1 2>/dev/null &

            set loglines [count_log_lines 0]
            set failed [catch {r test.rdbsave $failed_rdb}]
            assert_equal 1 $failed
            verify_log_message 0 "*Write error while saving DB to the disk(rdbSaveRio):*" $loglines
            assert_equal 0 [file exists $failed_rdb]
            assert_equal PONG [r ping]
        } {
            file delete -force $failed_rdb
            catch {r config set rdbcompression yes}
            catch {r flushdb}
        }
    }

    test "Module rdbloadsave with callback stream" {
        set dir [lindex [r config get dir] 1]
        assert_equal OK [r test.sanity_cb]
        r flushdb
        populate 1000 cb 100
        r expire cb0 10000
        r test.cb_stat io_calls

        assert_equal OK [r test.rdbsave_cb $dir/cb.rdb]
        assert_equal 1 [r test.cb_stat last_commit]
        r flushdb
        assert_equal OK [r test.rdbload_cb $dir/cb.rdb]
        assert_equal 1000 [r dbsize]
        assert_morethan [r ttl cb0] 0

        # The small reads and writes of the RDB code are buffered.
        assert_lessthan [r test.cb_stat io_calls] 20

        # Same format as a file stream.
        assert_equal OK [r test.rdbsave $dir/file.rdb]
        assert_equal OK [r test.rdbload_cb $dir/file.rdb]
        assert_equal 1000 [r dbsize]
    }

    test "Module rdbsave with callback stream keeps the previous object on errors" {
        set dir [lindex [r config get dir] 1]
        r flushdb
        r set k v1
        assert_equal OK [r test.rdbsave_cb $dir/abort.rdb]
        r set k v2

        assert_error {*Input/output error*} {r test.rdbsave_cb $dir/abort.rdb failwrite 0}
        assert_equal 0 [r test.cb_stat last_commit]
        assert_error {*Input/output error*} {r test.rdbsave_cb $dir/abort.rdb failflush}
        assert_equal 0 [r test.cb_stat last_commit]
        assert_error {*Input/output error*} {r test.rdbsave_cb $dir/abort.rdb failclose}

        assert_equal OK [r test.rdbload_cb $dir/abort.rdb]
        assert_equal v1 [r get k]
    }

    test "Module rdbsave with callback stream doesn't process events while saving" {
        set dir [lindex [r config get dir] 1]
        r flushdb
        r set k v
        file delete $dir/yield.rdb.started $dir/yield.rdb.go
        # ValkeyModule_Yield() would process events after this threshold.
        with_config busy-reply-threshold 100 {
            set rd [valkey_deferring_client]
            set rd2 [valkey_deferring_client]
            $rd test.rdbsave_cb $dir/yield.rdb yieldwrite
            wait_for_condition 50 100 {
                [file exists $dir/yield.rdb.started]
            } else {
                fail "The save didn't start"
            }
            # Sent while the write callback yields, after the threshold.
            $rd2 ping
            after 300
            close [open $dir/yield.rdb.go w]
            # Not rejected with -BUSY, but processed once the save is done.
            assert_equal OK [$rd read]
            assert_equal PONG [$rd2 read]
            $rd close
            $rd2 close
        }

        # The callbacks of a save or a load can't save or load either.
        assert_equal OK [r test.rdbsave_cb $dir/nested_cb.rdb nested]
        assert_match {*busy*busy*} [r test.cb_stat nested_result]
        assert_equal v [r get k]
        assert_equal OK [r test.rdbload_cb $dir/nested_cb.rdb nested]
        assert_match {*busy*busy*} [r test.cb_stat nested_result]
        assert_equal v [r get k]
        assert {![file exists $dir/nested.rdb]}
    }

    test "Module rdbsave with callback stream in a module fork" {
        set dir [lindex [r config get dir] 1]
        r flushdb
        r set k v1
        populate 10 fork 10

        with_config rdb-key-save-delay 100000 {
            assert_equal OK [r test.rdbsave_cb $dir/fork.rdb fork]
            r set k v2
            wait_for_condition 50 100 {
                [s module_fork_in_progress] == 0
            } else {
                fail "Module fork didn't finish"
            }
        }

        assert_equal OK [r test.rdbload_cb $dir/fork.rdb]
        assert_equal v1 [r get k]
    }

    foreach algo {no lz4} {
        test "Module rdbload with bad callback stream fails without exiting (rdbcompression $algo)" {
            set dir [lindex [r config get dir] 1]
            with_config rdbcompression $algo {
                r flushdb
                r set big [string repeat a 100000]
                assert_equal OK [r test.rdbsave_cb $dir/bad.rdb]
                set size [file size $dir/bad.rdb]

                file copy -force $dir/bad.rdb $dir/corrupt.rdb
                set fd [open $dir/corrupt.rdb r+]
                fconfigure $fd -translation binary
                seek $fd [expr {$size / 2}]
                puts -nonewline $fd "b"
                close $fd

                # A timer has no current client, so a file stream would exit here.
                foreach {file opts} [list bad.rdb [list limit [expr {$size / 2}]] corrupt.rdb {}] {
                    assert_equal OK [r test.rdbload_cb $dir/$file {*}$opts timer]
                    wait_for_condition 50 100 {
                        [r test.cb_stat timer_result] ne "pending"
                    } else {
                        fail "Timer load didn't finish"
                    }
                    assert_match {*Input/output error*} [r test.cb_stat timer_result]
                    assert_equal 0 [r test.cb_stat last_commit]
                }

                # A stream that ends early, e.g. in the middle of a compressed
                # frame, is a short read rather than a corrupt RDB.
                set loglines [count_log_lines 0]
                assert_error {*Input/output error*} {r test.rdbload_cb $dir/bad.rdb limit [expr {$size / 2}]}
                set lines [exec tail -n +[expr {$loglines + 1}] [srv 0 stdout]]
                assert {![string match "*Corrupt*" $lines]}
                # The server doesn't exit, so the log doesn't say it does.
                assert {[string match "*Short read or OOM loading DB.*" $lines]}
                assert {![string match -nocase "*aborting now*" $lines]}

                # From a command, the error is still logged at the default level.
                set loglines [count_log_lines 0]
                assert_error {*Input/output error*} {r test.rdbload_cb $dir/corrupt.rdb}
                verify_log_message 0 "*Failure loading rdb format from stream*" $loglines

                set loglines [count_log_lines 0]
                assert_equal OK [r test.rdbload_cb $dir/bad.rdb]
                if {$algo ne "no"} {
                    verify_log_message 0 "*Loading compressed RDB (algo=$algo)*" $loglines
                }
                assert_equal 100000 [r strlen big]
            }
        }
    }

    test "Module rdbload with callback stream doesn't restart the AOF after a failure" {
        # The server restarts with appendonly yes from its config file, to
        # check what the AOF has after a failed load.
        start_server [list overrides [list loadmodule $testmodule appendonly yes]] {
            set dir [lindex [r config get dir] 1]
            waitForBgrewriteaof r
            populate 1000 aoffail 10
            set digest [r debug digest]
            assert_equal OK [r test.rdbsave_cb $dir/aoffail.rdb]
            set size [file size $dir/aoffail.rdb]

            # A stream that fails before the dataset was cleared keeps the AOF.
            set rewrites [s aof_rewrites]
            assert_error {*Input/output error*} {r test.rdbload_cb $dir/aoffail.rdb limit 0}
            assert_equal 1000 [r dbsize]
            assert_equal {appendonly yes} [r config get appendonly]
            waitForBgrewriteaof r
            assert_equal [expr {$rewrites + 1}] [s aof_rewrites]

            # Once the dataset was cleared, the AOF still has the previous dataset,
            # instead of a rewrite of the partially loaded one.
            set rewrites [s aof_rewrites]
            set loglines [count_log_lines 0]
            assert_error {*Input/output error*} {r test.rdbload_cb $dir/aoffail.rdb limit [expr {$size / 2}]}
            assert_equal 0 [s aof_enabled]
            assert_equal {appendonly no} [r config get appendonly]
            assert_equal $rewrites [s aof_rewrites]
            verify_log_message 0 "*AOF stays disabled after the failed load of the module stream*" $loglines

            # The previous dataset is loaded from the AOF after a restart.
            restart_server 0 true false
            assert_equal 1000 [r dbsize]
            assert_equal $digest [r debug digest]
        }
    }

    test "Module rdbload with callback stream keeps the AOF the close callback enabled" {
        set dir [lindex [r config get dir] 1]
        r flushdb
        r set k v
        assert_equal OK [r test.rdbsave_cb $dir/aofon.rdb]
        with_config appendonly no {
            # The AOF is enabled by the close callback, so it isn't started again.
            assert_equal OK [r test.rdbload_cb $dir/aofon.rdb aofon]
            assert_equal 1 [s aof_enabled]
            assert_equal v [r get k]
            waitForBgrewriteaof r
        }
    }

    test "Module rdbload with callback stream fails on a database the server doesn't have" {
        set dir [lindex [r config get dir] 1]
        r select 12
        r set k v
        assert_equal OK [r test.rdbsave_cb $dir/db12.rdb]
        r flushdb
        r select 9

        start_server [list overrides [list loadmodule $testmodule databases 10]] {
            r set k v
            set loglines [count_log_lines 0]
            assert_error {*Input/output error*} {r test.rdbload_cb $dir/db12.rdb}
            assert_equal PONG [r ping]
            verify_log_message 0 "*Can't load DB 12 from the stream, this server has only 10 databases*" $loglines
            assert_equal 0 [count_log_message 0 "Exiting"]
        }
    }

    test "Unload the module - rdbloadsave" {
        assert_equal {OK} [r module unload rdbloadsave]
    }

    tags {repl} {
        test {Module rdbloadsave on master and replica} {
            start_server [list overrides [list loadmodule "$testmodule"]] {
                set replica [srv 0 client]
                set replica_host [srv 0 host]
                set replica_port [srv 0 port]
                start_server [list overrides [list loadmodule "$testmodule"]] {
                    set master [srv 0 client]
                    set master_host [srv 0 host]
                    set master_port [srv 0 port]

                    $master set x 10000

                    # Start the replication process...
                    $replica replicaof $master_host $master_port

                    wait_for_condition 100 100 {
                        [status $master sync_full] == 1
                    } else {
                        fail "Master <-> Replica didn't start the full sync"
                    }

                    # RM_RdbSave() is allowed on replicas
                    assert_equal OK [$replica test.rdbsave rep.rdb]

                    # RM_RdbLoad() is not allowed on replicas
                    assert_error {*supported*} {$replica test.rdbload rep.rdb}

                    assert_equal OK [$master test.rdbsave master.rdb]
                    $master set x 20000

                    wait_for_condition 100 100 {
                        [$replica get x] == 20000
                    } else {
                        fail "Replica didn't get the update"
                    }

                    # Loading RDB on master will drop replicas
                    assert_equal OK [$master test.rdbload master.rdb]

                    wait_for_condition 100 100 {
                        [status $master sync_full] == 2
                    } else {
                        fail "Master <-> Replica didn't start the full sync"
                    }

                    wait_for_condition 100 100 {
                        [$replica get x] == 10000
                    } else {
                        fail "Replica didn't get the update"
                    }
                }
            }
        }
    }
}
