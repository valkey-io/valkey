# Tests for reply blocking durability feature
# This test suite covers the synchronous replication functionality
# that blocks client responses until durability providers acknowledge writes.
#
# Tests are parameterized over provider_mode:
#   replica - unblock via replica replication ack
#   aof     - unblock via AOF appendfsync=always (automatic in beforeSleep)

foreach provider_mode {replica aof} {

    if {$provider_mode eq "replica"} {
        set server_overrides {sync-replication yes}
    } else {
        # Start with appendfsync no so writes block until we explicitly
        # trigger a fsync by switching to appendfsync always
        set server_overrides {sync-replication yes appendonly yes appendfsync no}
    }

    start_server [list tags {"repl durability external:skip"} overrides $server_overrides] {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        start_server {} {
            set replica [srv 0 client]
            set replica_host [srv 0 host]
            set replica_port [srv 0 port]

            # Helper: trigger durability acknowledgement.
            #   replica mode: connect replica and wait for replication ack
            #   aof mode: switch appendfsync to always, which triggers a fsync in beforeSleep
            proc unblock_with_provider {} {
                upvar provider_mode provider_mode
                upvar primary primary
                upvar primary_host primary_host
                upvar primary_port primary_port
                upvar replica replica
                upvar replica_host replica_host
                upvar replica_port replica_port

                if {$provider_mode eq "replica"} {
                    $replica replicaof $primary_host $primary_port
                    wait_replica_online $primary
                    wait_replica_acked_ofs $primary $replica $replica_host $replica_port
                } else {
                    # Switch from appendfsync no -> always to trigger AOF fsync
                    $primary config set appendfsync always
                    # Issue a PING to force a beforeSleep cycle that fsyncs the AOF
                    $primary ping
                }
            }

            # Helper: reset provider state after a blocking test.
            #   replica mode: disconnect replica
            #   aof mode: switch appendfsync back to no (so next test blocks again)
            proc cleanup_provider {} {
                upvar provider_mode provider_mode
                upvar primary primary
                upvar replica replica

                if {$provider_mode eq "replica"} {
                    $replica replicaof no one
                    wait_for_condition 50 100 {
                        [llength [$primary client list type replica]] == 0
                    } else {
                        fail "Primary didn't notice replica disconnect"
                    }
                } else {
                    # Reset to appendfsync no so the next write will block
                    $primary config set appendfsync no
                }
            }

            # ==================== Write blocking tests ====================

            test "($provider_mode) Sync replication blocks replies until provider acks" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                set rd [valkey_deferring_client -1]
                $rd set durable:blocked value

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                assert_equal "OK" [$rd read]
                $rd close

                cleanup_provider
            }

            test "($provider_mode) Sync replication blocks EXEC replies until provider acks" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                set rd [valkey_deferring_client -1]
                $rd multi
                $rd set durable:multi value

                assert_equal "OK" [$rd read]
                assert_equal "QUEUED" [$rd read]

                $rd exec
                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                assert_equal {OK} [$rd read]
                $rd close

                cleanup_provider
            }

            test "($provider_mode) Sync replication blocks only written keys in EXEC" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                set rd [valkey_deferring_client -1]
                $rd multi
                $rd set durable:multi-dirty value
                $rd get durable:multi

                assert_equal "OK" [$rd read]
                assert_equal "QUEUED" [$rd read]
                assert_equal "QUEUED" [$rd read]

                set reader [valkey_client]
                assert_equal {value} [$reader get durable:multi]

                $rd exec
                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                assert_equal {OK value} [$rd read]
                $rd close

                cleanup_provider
            }

            test "($provider_mode) Lua script write blocks replies until provider acks" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                assert_equal "OK" [$primary set durable:lua-clean clean]

                set rd [valkey_deferring_client -1]
                $rd eval {redis.call('set', KEYS[1], ARGV[1]); return redis.call('get', KEYS[2])} 2 durable:lua-dirty durable:lua-clean value

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                set reader [valkey_client]
                assert_equal {clean} [$reader get durable:lua-clean]

                unblock_with_provider

                assert_equal {clean} [$rd read]
                $rd close

                cleanup_provider
            }

            test "($provider_mode) Lua script error after partial write still blocks" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                set rd [valkey_deferring_client -1]
                $rd eval {redis.call('set', KEYS[1], 'written'); error('deliberate error')} 1 durable:lua-error-key

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                catch {$rd read} err
                assert_match "*deliberate error*" $err
                $rd close

                cleanup_provider
            }

            # ==================== Non-blocking tests ====================

            test "($provider_mode) EVAL_RO should not block replies" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                assert_equal "OK" [$primary set durable:eval-ro-key hello]

                set rd [valkey_deferring_client -1]
                $rd eval_ro {return redis.call('get', KEYS[1])} 1 durable:eval-ro-key

                assert_equal "hello" [$rd read]
                $rd close
            }

            test "($provider_mode) MULTI/EXEC with DISCARD does not block" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                set rd [valkey_deferring_client -1]
                $rd multi
                assert_equal "OK" [$rd read]

                $rd set durable:discard-key value
                assert_equal "QUEUED" [$rd read]

                $rd discard
                assert_equal "OK" [$rd read]

                $rd get durable:discard-key
                assert_equal "" [$rd read]
                $rd close
            }

            test "($provider_mode) MULTI/EXEC with no writes does not block" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                assert_equal "OK" [$primary set durable:nowrite-key existing]

                set rd [valkey_deferring_client -1]
                $rd multi
                assert_equal "OK" [$rd read]

                $rd get durable:nowrite-key
                assert_equal "QUEUED" [$rd read]

                $rd ping
                assert_equal "QUEUED" [$rd read]

                $rd exec
                assert_equal {existing PONG} [$rd read]
                $rd close
            }

            test "($provider_mode) Admin commands are never blocked" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                set rd [valkey_deferring_client -1]

                $rd ping
                assert_equal "PONG" [$rd read]

                $rd info server
                set info [$rd read]
                assert_match "*valkey_version*" $info

                $rd dbsize
                set dbsize [$rd read]
                assert {[string is integer $dbsize]}

                $rd close
            }

            test "($provider_mode) Read-only commands on clean keys are not blocked" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                assert_equal "OK" [$primary config set sync-replication no]
                assert_equal "OK" [$primary set durable:clean-key cleanvalue]
                assert_equal "OK" [$primary config set sync-replication yes]

                set rd [valkey_deferring_client -1]
                $rd get durable:clean-key
                assert_equal "cleanvalue" [$rd read]
                $rd close
            }

            test "($provider_mode) Sync replication disabled - writes return immediately (regression)" {
                assert_equal "OK" [$primary config set sync-replication no]
                assert_equal "no" [lindex [$primary config get sync-replication] 1]

                set rd [valkey_deferring_client -1]
                $rd set durable:norep-key value
                assert_equal "OK" [$rd read]

                $rd get durable:norep-key
                assert_equal "value" [$rd read]

                $rd multi
                assert_equal "OK" [$rd read]
                $rd set durable:norep-key2 value2
                assert_equal "QUEUED" [$rd read]
                $rd exec
                assert_equal {OK} [$rd read]

                $rd close
                assert_equal "OK" [$primary config set sync-replication yes]
            }

            # ==================== Multiple clients ====================

            test "($provider_mode) Multiple concurrent writers block independently" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                set wr1 [valkey_deferring_client -1]
                set wr2 [valkey_deferring_client -1]

                $wr1 set durable:concurrent-1 val1
                $wr2 set durable:concurrent-2 val2

                set fd1 [$wr1 channel]
                set fd2 [$wr2 channel]
                fconfigure $fd1 -blocking 0
                fconfigure $fd2 -blocking 0
                set early1 [read $fd1]
                set early2 [read $fd2]
                fconfigure $fd1 -blocking 1
                fconfigure $fd2 -blocking 1
                assert_equal "" $early1
                assert_equal "" $early2

                unblock_with_provider

                assert_equal "OK" [$wr1 read]
                assert_equal "OK" [$wr2 read]

                $wr1 close
                $wr2 close

                cleanup_provider
            }

            test "($provider_mode) Write then read on same client preserves reply ordering" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                set rd [valkey_deferring_client -1]
                $rd set durable:ordering-key orderval
                $rd get durable:ordering-key

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                assert_equal "OK" [$rd read]
                assert_equal "orderval" [$rd read]
                $rd close

                cleanup_provider
            }

            # ==================== Database-level commands ====================

            test "($provider_mode) FLUSHDB inside MULTI/EXEC blocks entire database" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                assert_equal "OK" [$primary config set sync-replication no]
                assert_equal "OK" [$primary set durable:flush-pre existing]
                assert_equal "OK" [$primary config set sync-replication yes]

                set rd [valkey_deferring_client -1]
                $rd multi
                assert_equal "OK" [$rd read]

                $rd flushdb
                assert_equal "QUEUED" [$rd read]

                $rd exec
                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                assert_equal {OK} [$rd read]
                $rd close

                cleanup_provider
            }

            test "($provider_mode) FLUSHALL blocks write reply until provider acks" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                assert_equal "OK" [$primary config set sync-replication no]
                assert_equal "OK" [$primary set durable:flushall-key value]
                assert_equal "OK" [$primary config set sync-replication yes]

                set rd [valkey_deferring_client -1]
                $rd flushall

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                assert_equal "OK" [$rd read]
                $rd close

                cleanup_provider
            }

            test "($provider_mode) FLUSHALL inside MULTI/EXEC blocks all databases" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                assert_equal "OK" [$primary config set sync-replication no]
                assert_equal "OK" [$primary set durable:flushall-multi-key value]
                assert_equal "OK" [$primary config set sync-replication yes]

                set rd [valkey_deferring_client -1]
                $rd multi
                assert_equal "OK" [$rd read]

                $rd flushall
                assert_equal "QUEUED" [$rd read]

                $rd exec
                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                assert_equal {OK} [$rd read]
                $rd close

                cleanup_provider
            }

            test "($provider_mode) COPY cross-database blocks write reply" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                assert_equal "OK" [$primary config set sync-replication no]
                assert_equal "OK" [$primary set durable:copy-src srcvalue]
                assert_equal "OK" [$primary config set sync-replication yes]

                set rd [valkey_deferring_client -1]
                $rd copy durable:copy-src durable:copy-dst db 1

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                assert_equal 1 [$rd read]
                $rd close

                cleanup_provider
            }

            test "($provider_mode) SWAPDB blocks write reply until provider acks" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                assert_equal "OK" [$primary config set sync-replication no]
                assert_equal "OK" [$primary set durable:swap-db0 db0val]
                $primary select 1
                assert_equal "OK" [$primary set durable:swap-db1 db1val]
                $primary select 0
                assert_equal "OK" [$primary config set sync-replication yes]

                set rd [valkey_deferring_client -1]
                $rd swapdb 0 1

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                assert_equal "OK" [$rd read]
                $rd close

                # Swap back to restore state
                $primary swapdb 0 1

                cleanup_provider
            }

            test "($provider_mode) MOVE blocks write reply until provider acks" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                assert_equal "OK" [$primary config set sync-replication no]
                assert_equal "OK" [$primary set durable:move-key moveval]
                assert_equal "OK" [$primary config set sync-replication yes]

                set rd [valkey_deferring_client -1]
                $rd move durable:move-key 1

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                assert_equal 1 [$rd read]
                $rd close

                cleanup_provider
            }

            test "($provider_mode) MULTI/EXEC with SELECT writes to multiple databases blocks" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                set rd [valkey_deferring_client -1]
                $rd multi
                assert_equal "OK" [$rd read]

                $rd set durable:multidb-key0 val0
                assert_equal "QUEUED" [$rd read]

                $rd select 1
                assert_equal "QUEUED" [$rd read]

                $rd set durable:multidb-key1 val1
                assert_equal "QUEUED" [$rd read]

                $rd select 0
                assert_equal "QUEUED" [$rd read]

                $rd exec
                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                assert_equal {OK OK OK OK} [$rd read]
                $rd close

                cleanup_provider
            }

            # ==================== Function store ====================

            test "($provider_mode) FUNCTION LOAD blocks reply until provider acks" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                set rd [valkey_deferring_client -1]
                $rd function load "#!lua name=durtest\nserver.register_function('durfunc', function() return 'hello' end)"

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                assert_equal "durtest" [$rd read]
                $rd close

                cleanup_provider
            }

            test "($provider_mode) FUNCTION DELETE blocks reply until provider acks" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                set rd [valkey_deferring_client -1]
                $rd function delete durtest

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                assert_equal "OK" [$rd read]
                $rd close

                cleanup_provider
            }

            # ==================== Dirty key reads ====================

            test "($provider_mode) Sync replication blocks reads on dirty keys" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                set writer [valkey_deferring_client -1]
                $writer client reply off
                $writer set durable:blocked dirty

                set rd [valkey_deferring_client -1]
                $rd get durable:blocked

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                assert_equal "dirty" [$rd read]
                $rd close

                cleanup_provider
            }

            # ==================== Client disconnect stats ====================

            test "($provider_mode) Client disconnect while blocked updates stats" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                set rd [valkey_deferring_client -1]
                $rd set durable:disconnect-test value

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                assert_equal "" $early_reply

                $rd close

                after 200

                set info [$primary info all]
                assert_match "*sync_repl_clients_waiting_ack:0*" $info
            }

            # ==================== Toggle / config changes ====================

            test "($provider_mode) Sync replication toggling disables reply blocking" {
                assert_equal "OK" [$primary config set sync-replication no]
                assert_equal "no" [lindex [$primary config get sync-replication] 1]

                set writer [valkey_deferring_client -1]
                $writer client reply off
                $writer set durable:toggle value

                set rd [valkey_deferring_client -1]
                $rd get durable:toggle
                assert_equal "value" [$rd read]

                $rd close
                assert_equal "OK" [$primary config set sync-replication yes]
            }

            test "($provider_mode) Disabling sync replication unblocks pending replies" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                set rd [valkey_deferring_client -1]
                $rd set durable:toggle-blocked value

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                assert_equal "" $early_reply

                assert_equal "OK" [$primary config set sync-replication no]

                set raw_reply ""
                set got_reply 0
                for {set i 0} {$i < 50} {incr i} {
                    append raw_reply [read $fd]
                    if {[string match "*\r\n" $raw_reply]} {
                        set got_reply 1
                        break
                    }
                    after 100
                }
                if {!$got_reply} {
                    fail "Reply didn't unblock after disabling sync replication"
                }
                fconfigure $fd -blocking 1
                assert_match "+OK*" $raw_reply

                assert_equal "OK" [$primary config set sync-replication yes]
            }

            test "($provider_mode) INFO reports sync replication stats" {
                set info [$primary info all]
                assert_match "*sync_replication_enabled:1*" $info
                assert_match "*sync_repl_primary_repl_offset:*" $info
                assert_match "*sync_repl_previous_acked_offset:*" $info
            }

            # ==================== Failover tests (must be last  changes roles) ====================

            test "($provider_mode) Failover disconnects clients waiting for ack" {
                assert_equal "yes" [lindex [$primary config get sync-replication] 1]

                set rd [valkey_deferring_client -1]
                $rd client setname durability-waiter
                $rd read
                $rd set durable:failover value

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                assert_equal "" $early_reply
                fconfigure $fd -blocking 1

                $primary replicaof $replica_host $replica_port

                catch {$rd read} err
                assert_match {*I/O error*} $err
            }

            test "($provider_mode) Demoted primary returns ERR on dirty data" {
                set reader [valkey_client -1]
                catch {$reader get durable:failover} err
                assert_equal "ERR Accessed data unavailable to be served" $err
            }
        }
    }
}