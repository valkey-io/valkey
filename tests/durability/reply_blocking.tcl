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
    }
}
