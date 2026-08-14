# Tests for reply blocking durability feature
# This test suite covers the synchronous replication functionality
# that blocks client responses until durability providers acknowledge writes.
#
# Tests are parameterized over provider_mode:
#   replica - TODO
#   aof     - unblock via AOF appendfsync=always (automatic in beforeSleep)

foreach provider_mode {aof} {

    if {$provider_mode eq "replica"} {
        set server_overrides {appendonly yes appendfsync always bio-aof-offload-enabled yes}
    } else {
        # Durability is implied by appendonly + appendfsync always.
        # We use DEBUG reply-blocking-pause/resume to control blocking
        # instead of toggling appendfsync, which avoids the issue where the
        # provider reports as disabled when appendfsync != always.
        set server_overrides {appendonly yes appendfsync always bio-aof-offload-enabled yes}
    }

    start_server [list tags {"repl durability external:skip"} overrides $server_overrides] {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        start_server {} {
            set replica [srv 0 client]
            set replica_host [srv 0 host]
            set replica_port [srv 0 port]

            # Helper: put the provider into a state where writes will block.
            #   replica mode: ensure no replica is connected (so no one acks writes)
            #   aof mode: pause the AOF provider so fsynced offsets are not advanced
            proc pause_provider {} {
                upvar provider_mode provider_mode
                upvar primary primary
                upvar replica replica

                if {$provider_mode eq "replica"} {
                    # Disconnect any existing replica so the next write has no one to ack it
                    $replica replicaof no one
                    wait_for_condition 50 100 {
                        [llength [$primary client list type replica]] == 0
                    } else {
                        fail "Primary didn't notice replica disconnect"
                    }
                } else {
                    # Pause the AOF provider so the next write will block
                    $primary DEBUG reply-blocking-pause aof
                }
            }

            # Helper: trigger durability acknowledgement, unblocking pending replies.
            #   replica mode: connect replica and wait for replication ack
            #   aof mode: resume the AOF provider and ping to force a beforeSleep fsync
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
                    # Resume the AOF provider so it reports real fsynced offsets
                    $primary DEBUG reply-blocking-resume aof
                    # Issue a PING to force a beforeSleep cycle that fsyncs the AOF
                    $primary ping
                }
            }

            # ==================== bio-aof-offload-enabled config tests ====================

            test "bio-aof-offload-enabled can be toggled at runtime - $provider_mode" {
                # Server started with bio-aof-offload-enabled yes
                assert_equal [lindex [$primary config get bio-aof-offload-enabled] 1] "yes"

                $primary set key1 value1
                assert_equal [$primary get key1] "value1"

                $primary config set bio-aof-offload-enabled no
                assert_equal [lindex [$primary config get bio-aof-offload-enabled] 1] "no"

                $primary set key2 value2
                assert_equal [$primary get key2] "value2"

                # Re-enable for remaining tests
                $primary config set bio-aof-offload-enabled yes
                assert_equal [lindex [$primary config get bio-aof-offload-enabled] 1] "yes"
            }

            # ==================== Write blocking tests ====================

            test "($provider_mode) Sync replication blocks replies until provider acks" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]
                puts "durability blocks"
                pause_provider

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
            }

            test "($provider_mode) Sync replication blocks EXEC replies until provider acks" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]
                puts "durability blocks"

                pause_provider

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
            }

            test "($provider_mode) Sync replication blocks only written keys in EXEC" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]
                puts "durability only written keys in EXEC"

                # Pre-populate with durability off so the SET doesn't block
                assert_equal "OK" [$primary set durable:multi-clean clean]
                # Verify the pre-populated value is readable on the primary before EXEC

                pause_provider

                set rd [valkey_deferring_client -1]
                $rd multi
                $rd set durable:multi-dirty value
                $rd get durable:multi-clean

                assert_equal "OK" [$rd read]
                assert_equal "QUEUED" [$rd read]
                assert_equal "QUEUED" [$rd read]
                assert_equal {clean} [$primary get durable:multi-clean]


                $rd exec
                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                assert_equal {OK clean} [$rd read]
                $rd close
            }

            test "($provider_mode) Lua script write blocks replies until provider acks" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                # Pre-populate with sync-repl off so the SET doesn't block
                assert_equal "OK" [$primary config set appendfsync everysec]
                assert_equal "OK" [$primary set durable:lua-clean clean]
                assert_equal "OK" [$primary config set appendfsync always]

                pause_provider

                set rd [valkey_deferring_client -1]
                $rd eval {redis.call('set', KEYS[1], ARGV[1]); return redis.call('get', KEYS[2])} 2 durable:lua-dirty durable:lua-clean value

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                set reader [valkey_client -1]
                assert_equal {clean} [$reader get durable:lua-clean]

                unblock_with_provider

                assert_equal {clean} [$rd read]
                $rd close
            }

            test "($provider_mode) Lua script error after partial write still blocks" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                pause_provider

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
            }

            test "($provider_mode) Sync replication blocks reply held in the reply list until provider acks" {
                # Force the reply into the c->reply overflow list (not the static
                # buf) and use a ~100KB allowed prefix so it spans multiple blocks.
                set bigval [string repeat A 100000]
                $primary debug client-enforce-reply-list 1
                $primary set durable:committed2 $bigval
                unblock_with_provider
                pause_provider

                set rd [valkey_deferring_client -1]
                $rd get durable:committed2
                $rd set durable:pending2 y

                # Allowed prefix (the multi-block GET reply) must be released in full.
                assert_equal $bigval [$rd read]

                # Blocked suffix (SET reply) must still be withheld.
                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                assert_equal "OK" [$rd read]
                $rd close
                $primary debug client-enforce-reply-list 0
            }

            test "($provider_mode) Blocked reply stays withheld during re-entrant event processing (busy script)" {
                # Regression test for the re-entrant beforeSleep branch guarded by
                # ProcessingEventsWhileBlocked. A busy Lua script drives the server
                # into processEventsWhileBlocked(), which re-enters beforeSleep and
                # calls handleClientsWithPendingWrites(). This test proves a
                # not-yet-durable write reply is NOT flushed on that path: durability
                # is enforced solely by the per-client reply-blocking boundary, not by
                # skipping the flush entirely.
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                # Lower the busy threshold so the spinning script re-enters the event
                # loop quickly, then restore it at the end.
                set saved_limit [lindex [$primary config get busy-reply-threshold] 1]
                $primary config set busy-reply-threshold 10

                pause_provider

                # Writer issues a command whose reply must block until durable.
                set writer [valkey_deferring_client -1]
                $writer set durable:reentrant pending

                # Wait until the write is registered as blocked (waiting for ack),
                # so it is queued for flushing when the re-entrant path runs.
                wait_for_condition 50 100 {
                    [string match "*reply_blocking_clients_waiting_ack:1*" [$primary info debug]]
                } else {
                    fail "Writer's reply never entered the blocked state"
                }

                # Busy read-only script forces the server into processEventsWhileBlocked.
                set busy [valkey_deferring_client -1]
                $busy eval {while true do end} 0

                # Confirm the re-entrant event loop is actually running: while busy,
                # the server answers other clients with -BUSY from beforeSleep re-entry.
                wait_for_condition 50 100 {
                    [catch {$primary ping} e] == 1 && [string match "BUSY*" $e]
                } else {
                    fail "Busy script did not enter processEventsWhileBlocked"
                }

                # Give the re-entrant beforeSleep several iterations to (not) flush it.
                after 200

                # The blocked reply must not have leaked out during the busy period.
                set fd [$writer channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                # Stop the busy script (read-only, so SCRIPT KILL is permitted).
                $primary script kill
                wait_for_condition 50 100 {
                    [catch {$primary ping} e] == 0
                } else {
                    fail "Could not kill the busy script"
                }
                catch {$busy read} _
                $busy close

                # Once durability advances, the previously-blocked reply is released.
                unblock_with_provider
                assert_equal "OK" [$writer read]
                $writer close

                $primary config set busy-reply-threshold $saved_limit
            }

            test "($provider_mode) Key-targeting reads block per-key, not on the global dirty offset" {
                # KEYSPACE_GLOBAL narrowing: commands that carry a key argument
                # (EXISTS/TYPE/TTL/...) must block only on their own key's offset,
                # not on the global offset just because some unrelated key is dirty.
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                # A committed, clean key to read later (written while blocking is off).
                $primary config set appendfsync everysec
                $primary set durable:kt-clean cleanval
                $primary config set appendfsync always

                pause_provider

                # Dirty an unrelated key so the keyspace has uncommitted data.
                set rd [valkey_deferring_client -1]
                $rd set durable:kt-dirty pending
                wait_for_condition 50 100 {
                    [string match "*reply_blocking_clients_waiting_ack:1*" [$primary info debug]]
                } else {
                    fail "writer's reply never entered the blocked state"
                }

                # Reads of the clean key must return immediately despite the dirty key.
                set reader [valkey_client -1]
                assert_equal 1 [$reader exists durable:kt-clean]
                assert_equal "string" [$reader type durable:kt-clean]
                assert_equal -1 [$reader ttl durable:kt-clean]
                $reader close

                # A read of the dirty key itself must still block until durable.
                set rd2 [valkey_deferring_client -1]
                $rd2 exists durable:kt-dirty
                set fd [$rd2 channel]
                fconfigure $fd -blocking 0
                assert_equal "" [read $fd]
                fconfigure $fd -blocking 1

                unblock_with_provider
                assert_equal 1 [$rd2 read]
                assert_equal "OK" [$rd read]
                $rd2 close
                $rd close
            }

            test "($provider_mode) Whole-keyspace commands still block while any data is dirty" {
                # KEYS/SCAN/RANDOMKEY have no key argument, so a not-yet-durable
                # write can add/remove a key they would report: they must block on the
                # global offset whenever anything is dirty.
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                pause_provider

                set rd [valkey_deferring_client -1]
                $rd set durable:global-dirty pending
                wait_for_condition 50 100 {
                    [string match "*reply_blocking_clients_waiting_ack:1*" [$primary info debug]]
                } else {
                    fail "writer's reply never entered the blocked state"
                }

                set rk [valkey_deferring_client -1]
                $rk keys *
                set fd [$rk channel]
                fconfigure $fd -blocking 0
                assert_equal "" [read $fd]
                fconfigure $fd -blocking 1

                unblock_with_provider
                assert_morethan [llength [$rk read]] 0
                assert_equal "OK" [$rd read]
                $rk close
                $rd close
            }

            test "($provider_mode) OBJECT HELP is never blocked" {
                # OBJECT HELP is static help text touching no keys; it must never block.
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                pause_provider

                set rd [valkey_deferring_client -1]
                $rd set durable:oh-dirty pending
                wait_for_condition 50 100 {
                    [string match "*reply_blocking_clients_waiting_ack:1*" [$primary info debug]]
                } else {
                    fail "writer's reply never entered the blocked state"
                }

                set reader [valkey_client -1]
                assert_match "*ENCODING*" [$reader object help]
                $reader close

                unblock_with_provider
                assert_equal "OK" [$rd read]
                $rd close
            }

            test "($provider_mode) Lazy-expired key deletion is tracked and blocks later reads until durable" {
                # A key deleted by lazy expiry is a background write (signalModifiedKey
                # with a NULL client). It must be tracked as uncommitted with no
                # per-function hook, so a later read blocks until the deletion is durable.
                assert_equal "always" [lindex [$primary config get appendfsync] 1]
                $primary debug set-active-expire 0

                # Commit a key, then make it logically expired (provider still acking).
                $primary set durable:lazyexp v
                $primary pexpire durable:lazyexp 1
                after 10

                pause_provider

                # Trigger lazy expiry: returns nil immediately (the triggering read is
                # not itself a write), deletes the key, and marks the deletion dirty.
                assert_equal {} [$primary get durable:lazyexp]

                # A later read of the now-deleted key must block until durable.
                set rd [valkey_deferring_client -1]
                $rd exists durable:lazyexp
                wait_for_condition 50 100 {
                    [string match "*reply_blocking_clients_waiting_ack:1*" [$primary info debug]]
                } else {
                    fail "lazy-expiry deletion was not tracked as uncommitted"
                }

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                assert_equal "" [read $fd]
                fconfigure $fd -blocking 1

                unblock_with_provider
                assert_equal 0 [$rd read]
                $rd close
                $primary debug set-active-expire 1
            }

            # ==================== Non-blocking tests ====================

            test "($provider_mode) EVAL_RO should not block replies" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                # Pre-populate with sync-repl off so the SET doesn't block
                assert_equal "OK" [$primary config set appendfsync everysec]
                assert_equal "OK" [$primary set durable:eval-ro-key hello]
                assert_equal "OK" [$primary config set appendfsync always]

                set rd [valkey_deferring_client -1]
                $rd eval_ro {return redis.call('get', KEYS[1])} 1 durable:eval-ro-key

                assert_equal "hello" [$rd read]
                $rd close
            }

            test "($provider_mode) MULTI/EXEC with DISCARD does not block" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

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
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                # Pre-populate with sync-repl off so the SET doesn't block
                assert_equal "OK" [$primary config set appendfsync everysec]
                assert_equal "OK" [$primary set durable:nowrite-key existing]
                assert_equal "OK" [$primary config set appendfsync always]

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
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

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
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                assert_equal "OK" [$primary config set appendfsync everysec]
                assert_equal "OK" [$primary set durable:clean-key cleanvalue]
                assert_equal "OK" [$primary config set appendfsync always]

                set rd [valkey_deferring_client -1]
                $rd get durable:clean-key
                assert_equal "cleanvalue" [$rd read]
                $rd close
            }

            test "($provider_mode) Sync replication disabled - writes return immediately (regression)" {
                assert_equal "OK" [$primary config set appendfsync everysec]
                assert_equal "everysec" [lindex [$primary config get appendfsync] 1]

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
                assert_equal "OK" [$primary config set appendfsync always]
            }

            # ==================== Multiple clients ====================

            test "($provider_mode) Multiple concurrent writers block independently" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                pause_provider

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
            }

            test "($provider_mode) Write then read on same client preserves reply ordering" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                pause_provider

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
            }

            # ==================== Database-level commands ====================

            test "($provider_mode) FLUSHDB inside MULTI/EXEC blocks entire database" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                assert_equal "OK" [$primary config set appendfsync everysec]
                assert_equal "OK" [$primary set durable:flush-pre existing]
                assert_equal "OK" [$primary config set appendfsync always]

                pause_provider

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
            }

            test "($provider_mode) FLUSHALL blocks write reply until provider acks" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                assert_equal "OK" [$primary set durable:flushall-key value]

                pause_provider

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
            }

            test "($provider_mode) FLUSHALL inside MULTI/EXEC blocks all databases" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                assert_equal "OK" [$primary config set appendfsync everysec]
                assert_equal "OK" [$primary set durable:flushall-multi-key value]
                assert_equal "OK" [$primary config set appendfsync always]

                pause_provider

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
            }

            test "($provider_mode) COPY cross-database blocks write reply" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                assert_equal "OK" [$primary config set appendfsync everysec]
                assert_equal "OK" [$primary set durable:copy-src srcvalue]
                assert_equal "OK" [$primary config set appendfsync always]

                pause_provider

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
            }

            test "($provider_mode) SWAPDB blocks write reply until provider acks" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                assert_equal "OK" [$primary set durable:swap-db0 db0val]
                $primary select 1
                assert_equal "OK" [$primary set durable:swap-db1 db1val]
                $primary select 0

                pause_provider

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

                # Swap back to restore state (with sync-repl off so it doesn't block)
                $primary config set appendfsync everysec
                $primary swapdb 0 1
                $primary config set appendfsync always
            }

            test "($provider_mode) MOVE blocks write reply until provider acks" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                $primary select 2
                $primary del durable:move-key
                $primary select 9
                assert_equal "OK" [$primary set durable:move-key moveval]
                assert_equal "OK" [$primary config set appendfsync always]

                pause_provider

                set rd [valkey_deferring_client -1]
                $rd move durable:move-key 2

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                assert_equal 1 [$rd read]
                $rd close
            }

            test "($provider_mode) MOVE/COPY to out-of-range destination DB does not crash" {
                # MOVE/COPY to an out-of-range DB is rejected but still reaches the
                # reply-blocking offset path; dbnum and -1 hit ASan's server.db redzone.
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                set dbnum [lindex [$primary config get databases] 1]
                $primary select 9
                assert_equal "OK" [$primary set durable:oob-move-key v]

                foreach idx [list $dbnum -1 100000] {
                    assert_error "*out of range*" {$primary move durable:oob-move-key $idx}
                    assert_equal "PONG" [$primary ping]
                    assert_error "*out of range*" {$primary copy durable:oob-move-key dst db $idx}
                    assert_equal "PONG" [$primary ping]
                }

                # Key untouched in its original DB after all rejected MOVEs/COPYs.
                assert_equal "v" [$primary get durable:oob-move-key]
                $primary del durable:oob-move-key
            }

            test "($provider_mode) MULTI/EXEC with SELECT writes to multiple databases blocks" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                pause_provider

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
            }

            # ==================== Function store ====================

            test "($provider_mode) FUNCTION LOAD blocks reply until provider acks" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                pause_provider

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
            }

            test "($provider_mode) FUNCTION DELETE blocks reply until provider acks" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                pause_provider

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
            }

            # ==================== Dirty key reads ====================

            test "($provider_mode) Sync replication blocks reads on dirty keys" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                pause_provider

                set writer [valkey_deferring_client -1]
                $writer client reply off
                $writer set durable:blocked dirty

                # Barrier: ensure the writer's SET is processed before the reader's GET.
                # Different TCP connections have no ordering guarantee.
                wait_for_condition 50 10 {
                    [catch {$primary debug object durable:blocked}] == 0
                } else {
                    fail "Writer's SET was not processed in time"
                }

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
            }

            test "($provider_mode) Pipelined non-blocking then blocking command does not leak blocked reply" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                pause_provider

                # Pipeline a non-blocking command (PING) followed by a blocking write (SET).
                # The PING reply is allowed to be sent, but the SET reply must be held.
                # Without proper write boundary capping, _writeToClient would send
                # both replies since they share the same c->buf.
                set rd [valkey_deferring_client -1]
                $rd ping
                $rd set pipe:boundary-key val1

                # Give the server time to process both commands and attempt the write
                after 100

                # Read whatever the server has sent — should be ONLY the PING reply
                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set partial [read $fd]
                fconfigure $fd -blocking 1

                # PING reply should be "+PONG\r\n" — no "+OK\r\n" from SET
                assert_match "*PONG*" $partial
                assert {![string match "*OK*" $partial]}

                unblock_with_provider

                # Now the SET reply should arrive
                assert_equal "OK" [$rd read]
                $rd close
            }

            # ==================== Client disconnect stats ====================

            test "($provider_mode) Client disconnect while blocked updates stats" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                pause_provider

                set rd [valkey_deferring_client -1]
                $rd set durable:disconnect-test value

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                assert_equal "" $early_reply

                $rd close

                after 200

                set info [$primary info debug]
                assert_match "*reply_blocking_clients_waiting_ack:0*" $info

                # Resume the provider so subsequent tests aren't affected
                unblock_with_provider
            }

            # ==================== Toggle / config changes ====================

            test "($provider_mode) Sync replication toggling disables reply blocking" {
                assert_equal "OK" [$primary config set appendfsync everysec]
                assert_equal "everysec" [lindex [$primary config get appendfsync] 1]

                set writer [valkey_deferring_client -1]
                $writer set durable:toggle value
                # Durability is off (everysec), so SET reply arrives immediately.
                # Reading it also serves as a barrier ensuring the key exists.
                assert_equal "OK" [$writer read]

                set rd [valkey_deferring_client -1]
                $rd get durable:toggle
                assert_equal "value" [$rd read]

                $rd close
                $writer close
                assert_equal "OK" [$primary config set appendfsync always]
            }

            test "($provider_mode) Disabling sync replication unblocks pending replies" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                pause_provider

                set rd [valkey_deferring_client -1]
                $rd set durable:toggle-blocked value

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                assert_equal "" $early_reply

                assert_equal "OK" [$primary config set appendfsync everysec]

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

                # Resume the provider so subsequent tests aren't affected
                # (disabling sync-replication unblocked the client but didn't resume the provider)
                $primary DEBUG reply-blocking-resume aof

                assert_equal "OK" [$primary config set appendfsync always]
            }

            test "($provider_mode) INFO reports sync replication stats" {
                set info [$primary info debug]
                assert_match "*reply_blocking_enabled:1*" $info
                assert_match "*reply_blocking_primary_repl_offset:*" $info
                assert_match "*reply_blocking_previous_acked_offset:*" $info
            }

            # ==================== Client tracking invalidation (deferred tasks) ====================

            test "($provider_mode) Key invalidation is deferred until provider acks - signalModifiedKey" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]
                puts "running key invalidation"
                # Set up a RESP3 tracking client that will receive invalidation messages
                set tracker [valkey_deferring_client -1]
                $tracker HELLO 3
                $tracker read ;# consume HELLO reply
                $tracker CLIENT TRACKING on
                $tracker read ;# consume TRACKING reply

                # Populate a key and cache it via GET on the tracking client
                $primary config set appendfsync everysec
                $primary set durable:track-key original
                $primary config set appendfsync always

                $tracker GET durable:track-key
                $tracker read ;# consume "original" — key is now tracked

                # Pause the provider so the next write's invalidation is deferred
                pause_provider

                # Write to the tracked key from a different client (fire-and-forget)
                set writer [valkey_deferring_client -1]
                $writer client reply off
                $writer set durable:track-key modified

                # Give the server a moment to process the write
                after 100

                # The tracking client should NOT have received an invalidation yet
                set tracker_fd [$tracker channel]
                fconfigure $tracker_fd -blocking 0
                set early_inval [read $tracker_fd]
                fconfigure $tracker_fd -blocking 1
                # No invalidation push should appear while provider is paused
                assert_equal "" $early_inval

                # Now unblock — this should trigger the deferred invalidation
                unblock_with_provider

                # Read the invalidation message from the tracking client
                # RESP3 push: [invalidate [key1 key2 ...]]
                set inval_msg [$tracker read]
                assert_match "*durable:track-key*" $inval_msg

                $tracker CLIENT TRACKING off
                $tracker read ;# consume reply
                $tracker close
                $writer close
            }

            test "($provider_mode) Flush invalidation is deferred until provider acks - signalFlushedDb" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]
                puts "running flush invalidation"

                # Set up a RESP3 BCAST tracking client to catch FLUSHDB invalidations
                set tracker [valkey_deferring_client -1]
                $tracker HELLO 3
                $tracker read ;# consume HELLO reply
                $tracker CLIENT TRACKING on BCAST
                $tracker read ;# consume TRACKING reply

                # Populate some keys so there's something to flush
                $primary config set appendfsync everysec
                $primary set durable:flush-track-a val_a
                $primary set durable:flush-track-b val_b
                $primary config set appendfsync always

                # Drain setup-SET invalidations. BCAST broadcasts them
                # asynchronously in beforeSleep, so poll until quiet instead of
                # using a racy fixed wait.
                set tracker_fd [$tracker channel]
                fconfigure $tracker_fd -blocking 0
                set quiet 0
                for {set i 0} {$i < 100} {incr i} {
                    if {[read $tracker_fd] eq ""} {
                        if {[incr quiet] >= 3} break
                    } else {
                        set quiet 0
                    }
                    after 20
                }
                fconfigure $tracker_fd -blocking 1

                # Pause the provider so the FLUSHDB invalidation is deferred
                pause_provider

                # Issue FLUSHDB from a fire-and-forget writer
                set writer [valkey_deferring_client -1]
                $writer client reply off
                $writer flushdb

                # Give the server time to process the command
                after 100

                # The tracking client should NOT have received flush invalidation yet
                fconfigure $tracker_fd -blocking 0
                set early_inval [read $tracker_fd]
                fconfigure $tracker_fd -blocking 1
                assert_equal "" $early_inval

                # Unblock — this triggers the deferred flush invalidation
                unblock_with_provider

                # The tracking client should now receive an invalidation
                # For FLUSHDB, the invalidation message contains NULL to indicate all keys
                # Use a polling read with timeout to avoid hanging if message doesn't arrive
                set inval_msg ""
                set got_inval 0
                fconfigure $tracker_fd -blocking 0
                for {set i 0} {$i < 50} {incr i} {
                    append inval_msg [read $tracker_fd]
                    if {[string match "*invalidate*" $inval_msg]} {
                        set got_inval 1
                        break
                    }
                    after 100
                }
                fconfigure $tracker_fd -blocking 1
                if {!$got_inval} {
                    fail "Flush invalidation message not received within timeout"
                }
                assert_match "*invalidate*" $inval_msg

                $tracker CLIENT TRACKING off
                $tracker read ;# consume reply
                $tracker close
                $writer close
            }

            # ==================== Keyspace notification deferral (deferred tasks) ====================

            test "($provider_mode) Keyspace notification is deferred until provider acks" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                # Enable keyspace notifications for all events
                $primary config set notify-keyspace-events KA

                # Subscribe to keyspace notifications
                set rd1 [valkey_deferring_client -1]
                assert_equal {1} [psubscribe $rd1 "__keyspace@*__:*"]

                # Pause the provider so keyspace notifications are deferred
                pause_provider

                # Write to a key from a fire-and-forget writer
                set writer [valkey_deferring_client -1]
                $writer client reply off
                $writer set durable:keyspace-deferred-key val

                # Give the server time to process the command
                after 100

                # The subscriber should NOT have received the notification yet
                set rd1_fd [$rd1 channel]
                fconfigure $rd1_fd -blocking 0
                set early_notif [read $rd1_fd]
                fconfigure $rd1_fd -blocking 1
                # No keyspace notification should appear while provider is paused
                assert_equal "" $early_notif

                # Now unblock — this should trigger the deferred keyspace notification
                unblock_with_provider

                # Read the keyspace notification
                set notif_msg [$rd1 read]
                assert_match "*set*" $notif_msg

                $rd1 close
                $writer close
                $primary config set notify-keyspace-events ""
            }

            test "($provider_mode) Keyspace notification fires immediately when sync replication disabled" {
                # Verify that without sync replication, keyspace events are NOT deferred
                $primary config set appendfsync everysec
                $primary config set notify-keyspace-events KA

                set rd1 [valkey_deferring_client]
                assert_equal {1} [psubscribe $rd1 *]
                r set foo bar
                assert_match "*set*" [$rd1 read]
                $rd1 close

                $primary config set notify-keyspace-events ""
                $primary config set appendfsync always
            }

            # ==================== Client tracking invalidation (existing) ====================

            test "($provider_mode) Key invalidation fires immediately when sync replication disabled" {
                # Verify that without sync replication, invalidations are NOT deferred
                $primary config set appendfsync everysec

                set tracker [valkey_deferring_client -1]
                $tracker HELLO 3
                $tracker read ;# consume HELLO reply
                $tracker CLIENT TRACKING on
                $tracker read ;# consume TRACKING reply

                $primary set durable:track-nodefer original

                $tracker GET durable:track-nodefer
                $tracker read ;# consume "original" — key is now tracked

                # Write to the tracked key — invalidation should fire immediately
                $primary set durable:track-nodefer changed

                # Should get the invalidation right away (no provider pause needed)
                set inval_msg [$tracker read]
                assert_match "*durable:track-nodefer*" $inval_msg

                $tracker CLIENT TRACKING off
                $tracker read ;# consume reply
                $tracker close

                $primary config set appendfsync always
            }

            # ==================== Durability provider edge cases ====================

            test "($provider_mode) Pause unknown provider returns error" {
                catch {$primary DEBUG reply-blocking-pause nonexistent} err
                assert_match "*No such reply-blocking provider*" $err
            }

            test "($provider_mode) Resume unknown provider returns error" {
                catch {$primary DEBUG reply-blocking-resume nonexistent} err
                assert_match "*No such reply-blocking provider*" $err
            }

            test "($provider_mode) Double pause is idempotent - writes still block" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                # Pause twice
                $primary DEBUG reply-blocking-pause aof
                $primary DEBUG reply-blocking-pause aof

                # Write should still block
                set rd [valkey_deferring_client -1]
                $rd set durable:double-pause-key val

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                # Resume once should unblock
                $primary DEBUG reply-blocking-resume aof
                $primary ping

                assert_equal "OK" [$rd read]
                $rd close
            }

            test "($provider_mode) Resume while not paused is harmless" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                # Resume when not paused should succeed without issue
                assert_equal "OK" [$primary DEBUG reply-blocking-resume aof]

                # Writes should still work normally
                set rd [valkey_deferring_client -1]
                $rd set durable:resume-noop-key val
                assert_equal "OK" [$rd read]
                $rd close
            }

            test "($provider_mode) Multiple writes while paused all unblock on resume" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                pause_provider

                set rd [valkey_deferring_client -1]
                $rd set durable:multi-write-1 val1
                $rd set durable:multi-write-2 val2
                $rd set durable:multi-write-3 val3

                set fd [$rd channel]
                fconfigure $fd -blocking 0
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                unblock_with_provider

                # All three replies should come through
                assert_equal "OK" [$rd read]
                assert_equal "OK" [$rd read]
                assert_equal "OK" [$rd read]
                $rd close
            }

            # ==================== Copy avoidance compatibility ====================

            test "($provider_mode) Blocked reply not released when io_last_written.data_len exceeds encoded boundary" {
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                pause_provider

                # Write a dirty key (fire-and-forget) so the next GET is blocked
                set writer [valkey_deferring_client -1]
                $writer client reply off
                $writer set durable:inject-key value

                # Barrier: ensure the writer's SET is processed before the reader's GET.
                wait_for_condition 50 10 {
                    [catch {$primary debug object durable:inject-key}] == 0
                } else {
                    fail "Writer's SET was not processed in time"
                }

                # Reader: issue GET on the dirty key — reply will be blocked
                set rd [valkey_deferring_client -1]

                # Get the reader's client ID for the DEBUG command
                $rd client id
                set reader_id [$rd read]

                $rd get durable:inject-key

                # Give server time to process and block the reply
                after 100

                # Inject the post-partial-write state using DEBUG.
                # This simulates what happens after IO threads write an
                # encoded (copy-avoided) buffer: data_len (decoded RESP bytes)
                # is much larger than bufpos (encoded buffer position).
                # bufpos=0 means "buffer not fully consumed" (incomplete write).
                # data_len=999999 is a large decoded byte count.
                #
                # The disallowed_byte_offset for the blocked reply is a small
                # number in encoded-buffer coordinates (~30 bytes).
                #
                # Bug (data_len):  999999 < ~30 → false → "no pending" →
                #   server thinks all data sent, removes write handler,
                #   reply leaks through!
                # Fix (bufpos):    0 < ~30 → true → "still pending" →
                #   reply stays blocked.
                $primary DEBUG set-io-last-written $reader_id 0 999999

                # Check: the reply must NOT leak through
                set fd [$rd channel]
                fconfigure $fd -blocking 0
                after 200
                set early_reply [read $fd]
                fconfigure $fd -blocking 1
                assert_equal "" $early_reply

                # Reset the injected state so normal writes work after unblock
                $primary DEBUG set-io-last-written $reader_id 0 0

                unblock_with_provider

                # Now the reply should come through
                assert_equal "value" [$rd read]
                $rd close
                $writer close
            }

            # ==================== Failover tests (must be last  changes roles) ====================

            test "($provider_mode) Failover disconnects clients waiting for ack" {
                # Ensure replica is in clean state for deterministic failover behavior.
                # In replica mode, earlier tests connected the replica and replicated data;
                # we flush it here so the demoted primary's dirty key tracking is preserved
                # correctly after failover (not overwritten by a full sync).
                $replica flushall
                assert_equal "always" [lindex [$primary config get appendfsync] 1]

                pause_provider

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

        }
    }
}
