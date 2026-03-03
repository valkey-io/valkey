# Tests for reply blocking durability feature
# This test suite covers the synchronous replication functionality
# that blocks client responses until replicas acknowledge writes

start_server {tags {"repl durability external:skip"} overrides {sync-replication yes}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    start_server {} {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]

        test "Sync replication blocks replies until replica acks" {
            assert_equal "yes" [lindex [$primary config get sync-replication] 1]

            set rd [valkey_deferring_client -1]
            $rd set durable:blocked value

            set fd [$rd channel]
            fconfigure $fd -blocking 0
            set early_reply [read $fd]
            fconfigure $fd -blocking 1
            assert_equal "" $early_reply

            $replica replicaof $primary_host $primary_port
            wait_replica_online $primary
            wait_replica_acked_ofs $primary $replica $replica_host $replica_port

            assert_equal "OK" [$rd read]

            $replica replicaof no one
            wait_for_condition 50 100 {
                [llength [$primary client list type replica]] == 0
            } else {
                fail "Primary didn't notice replica disconnect"
            }
        }

        test "Sync replication blocks EXEC replies until replica acks" {
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

            $replica replicaof $primary_host $primary_port
            wait_replica_online $primary
            wait_replica_acked_ofs $primary $replica $replica_host $replica_port

            assert_equal {OK} [$rd read]
        }

        test "Sync replication blocks only the keys we write to in EXEC replies until replica acks" {
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

            $replica replicaof $primary_host $primary_port
            wait_replica_online $primary
            wait_replica_acked_ofs $primary $replica $replica_host $replica_port

            assert_equal {OK value} [$rd read]
        }

        test "Sync replication blocks Lua script replies until replica acks" {
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

            $replica replicaof $primary_host $primary_port
            wait_replica_online $primary
            wait_replica_acked_ofs $primary $replica $replica_host $replica_port

            assert_equal {clean} [$rd read]
        }

        test "Sync replication blocks reads on dirty keys" {
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
            
            $replica replicaof $primary_host $primary_port
            wait_replica_online $primary
            wait_replica_acked_ofs $primary $replica $replica_host $replica_port

            assert_equal "dirty" [$rd read]
        }

        test "Sync replication toggling disables reply blocking" {
            assert_equal "OK" [$primary config set sync-replication no]
            assert_equal "no" [lindex [$primary config get sync-replication] 1]

            set writer [valkey_deferring_client -1]
            $writer client reply off
            $writer set durable:toggle value

            set rd [valkey_deferring_client -1]
            $rd get durable:toggle
            assert_equal "value" [$rd read]

            assert_equal "OK" [$primary config set sync-replication yes]
            assert_equal "yes" [lindex [$primary config get sync-replication] 1]
        }

        test "Disabling sync replication unblocks pending replies" {
            assert_equal "yes" [lindex [$primary config get sync-replication] 1]

            set rd [valkey_deferring_client -1]
            $rd set durable:toggle-blocked value

            set fd [$rd channel]
            fconfigure $fd -blocking 0
            set early_reply [read $fd]
            assert_equal "" $early_reply

            assert_equal "OK" [$primary config set sync-replication no]
            assert_equal "no" [lindex [$primary config get sync-replication] 1]

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
            assert_equal "yes" [lindex [$primary config get sync-replication] 1]
        }

        test "Failover disconnects clients waiting for ack" {
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

        test "Demoted primary returns ERR on dirty data" {
            set reader [valkey_client -1]
            catch {$reader get durable:failover} err
            assert_equal "ERR Accessed data unavailable to be served" $err
        }
    }
}

# Tests for AOF durability provider integration
# When sync-replication is enabled with appendfsync=always, the AOF provider
# participates in the durability consensus: replies are blocked until both
# the replica ACKs and the AOF fsync complete.
start_server {tags {"repl durability external:skip"} overrides {sync-replication yes appendonly yes appendfsync always}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    start_server {} {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]

        test "AOF durability provider - replies unblock after replica ack with AOF always" {
            # Ensure sync replication is enabled with AOF always
            assert_equal "yes" [lindex [$primary config get sync-replication] 1]
            assert_equal "always" [lindex [$primary config get appendfsync] 1]

            # Connect replica
            $replica replicaof $primary_host $primary_port
            wait_replica_online $primary

            # Write a key  with both replica + AOF providers, reply should
            # unblock once both have acknowledged
            set rd [valkey_deferring_client -1]
            $rd set durable:aof-test value

            wait_replica_acked_ofs $primary $replica $replica_host $replica_port

            # Reply should come through since both providers are satisfied
            assert_equal "OK" [$rd read]

            $rd close

            $replica replicaof no one
            wait_for_condition 50 100 {
                [llength [$primary client list type replica]] == 0
            } else {
                fail "Primary didn't notice replica disconnect"
            }
        }

        test "AOF durability provider - INFO reports sync replication stats" {
            # Verify that INFO output includes sync replication stats
            set info [$primary info all]
            assert_match "*sync_replication_enabled:1*" $info
            assert_match "*sync_repl_primary_repl_offset:*" $info
            assert_match "*sync_repl_previous_acked_offset:*" $info
        }

        test "AOF durability provider - write unblocks with AOF only (no replicas)" {
            # With no replicas connected and AOF always, the AOF provider alone
            # should drive durability progress
            assert_equal "yes" [lindex [$primary config get sync-replication] 1]
            assert_equal "always" [lindex [$primary config get appendfsync] 1]

            # No replicas connected  only AOF provider is active
            assert_equal 0 [llength [$primary client list type replica]]

            # Write should still succeed because AOF provider drives consensus
            set rd [valkey_deferring_client -1]
            $rd set durable:aof-only-test value

            # AOF fsync=always completes synchronously in beforeSleep,
            # so the reply should come through without needing a replica
            assert_equal "OK" [$rd read]
            $rd close
        }

        test "AOF durability provider - switching appendfsync to everysec disables AOF provider blocking" {
            # Disable AOF always  now only replica provider matters (which needs 0 acks currently)
            assert_equal "OK" [$primary config set appendfsync everysec]

            # Writes should immediately return since no durability provider requires blocking
            set rd [valkey_deferring_client -1]
            $rd set durable:aof-everysec-test value
            assert_equal "OK" [$rd read]
            $rd close

            # Restore AOF always
            assert_equal "OK" [$primary config set appendfsync always]
        }

        test "AOF durability provider - MULTI/EXEC unblocks after AOF fsync" {
            assert_equal "yes" [lindex [$primary config get sync-replication] 1]
            assert_equal "always" [lindex [$primary config get appendfsync] 1]

            set rd [valkey_deferring_client -1]
            $rd multi
            assert_equal "OK" [$rd read]

            $rd set durable:aof-multi key1
            assert_equal "QUEUED" [$rd read]

            $rd set durable:aof-multi2 key2
            assert_equal "QUEUED" [$rd read]

            $rd exec

            # With AOF always and no replicas, AOF provider drives consensus
            # EXEC reply should come through after AOF fsync
            assert_equal {OK OK} [$rd read]
            $rd close
        }
    }
}
