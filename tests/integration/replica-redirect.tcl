start_server {tags {needs:repl external:skip}} {
    start_server {} {
        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        set primary_pid [srv -1 pid]

        set replica_host [srv 0 host]
        set replica_port [srv 0 port]
        set replica_pid [srv 0 pid]

        r replicaof $primary_host $primary_port
        wait_replica_online $primary

        test {replica allow read command by default} {
            r get foo
        } {}

        test {replica reply READONLY error for write command by default} {
            assert_error {READONLY*} {r set foo bar}
        }

        test {replica redirect read and write command after CLIENT CAPA REDIRECT} {
            r client capa redirect
            assert_error "REDIRECT $primary_host:$primary_port" {r set foo bar}
            assert_error "REDIRECT $primary_host:$primary_port" {r get foo}
        }

        test {non-data access commands are not redirected} {
            r ping
        } {PONG}

        test {replica allow read command in READONLY mode} {
            r readonly
            r get foo
        } {}

        test {client paused before and during failover-in-progress} {
            set rd_blocking [valkey_deferring_client -1]
            $rd_blocking client capa redirect
            assert_match "OK" [$rd_blocking read]
            $rd_blocking brpop list 0

            wait_for_blocked_clients_count 1 100 10 -1

            pause_process $replica_pid

            r -1 failover to $replica_host $replica_port TIMEOUT 100 FORCE

            # Wait for primary to start failover
            wait_for_condition 50 100 {
                [s -1 master_failover_state] == "failover-in-progress"
            } else {
                fail "Failover from primary to replica did not timeout"
            }

            set rd [valkey_deferring_client -1]
            $rd client capa redirect
            assert_match "OK" [$rd read]
            $rd get foo

            # Reading and Writing clients paused during failover-in-progress, see more details in PR #871
            wait_for_blocked_clients_count 2 100 10 -1

            resume_process $replica_pid

            # Wait for failover to end
            wait_for_condition 50 100 {
                [s -1 master_failover_state] == "no-failover"
            } else {
                fail "Failover from primary to replica did not finish"
            }

            assert_match *master* [r role]
            assert_match *slave* [r -1 role]

            assert_error "REDIRECT $replica_host:$replica_port" {$rd read}
            assert_error "REDIRECT $replica_host:$replica_port" {$rd_blocking read}
            $rd close
            $rd_blocking close
        }

        test {responses in waiting-for-sync state} {
            r -1 replicaof no one
            r replicaof $primary_host $primary_port
            wait_replica_online $primary

            set rd_brpop_before [valkey_deferring_client -1]
            $rd_brpop_before client capa redirect
            assert_match "OK" [$rd_brpop_before read]
            $rd_brpop_before brpop list 0

            wait_for_blocked_clients_count 1 100 10 -1

            pause_process [srv 0 pid]

            set rd_wait [valkey_deferring_client -1]
            $rd_wait client capa redirect
            $rd_wait read ; # Consume the OK reply
            $rd_wait set foo bar
            $rd_wait read
            $rd_wait wait 1 0 ; # Blocks as we can't sync

            # XREAD is a reading command and thus, should
            # not be redirected if the client is read only.
            set rd_xread [valkey_deferring_client -1]
            $rd_xread client capa redirect
            assert_match "OK" [$rd_xread read]
            $rd_xread readonly
            assert_match "OK" [$rd_xread read]
            $rd_xread xread block 0 streams k 0

            wait_for_blocked_clients_count 3 100 10 -1

            r -1 failover

            # The replica is not synced and sleeps. Primary is waiting for
            # sync.
            assert_equal "waiting-for-sync" [s -1 master_failover_state]

            set rd_brpop_after [valkey_deferring_client -1]
            $rd_brpop_after client capa redirect
            assert_match "OK" [$rd_brpop_after read]
            $rd_brpop_after brpop list 0

            wait_for_blocked_clients_count 4 100 10 -1

            resume_process [srv 0 pid]

            # Wait for failover to end
            wait_for_condition 50 100 {
                [s -1 master_failover_state] == "no-failover"
            } else {
                fail "primary not in no-failover state"
            }

            assert_match *master* [r role]
            assert_match *slave* [r -1 role]

            # REDIRECT response shall be sent when the new primary is ready,
            # i.e. when unblocking all clients after failover
            assert_error "REDIRECT $replica_host:$replica_port" {$rd_brpop_before read}
            assert_error "REDIRECT $replica_host:$replica_port" {$rd_brpop_after read}

            # WAIT is unblocked in the main loop. Thus, it must have succeeded by now.
            assert_equal "1" [$rd_wait read]

            $rd_brpop_before close
            $rd_brpop_after close
            $rd_wait close

            # As a reading command for a readonly client, XREAD should still be blocked
            wait_for_blocked_clients_count 1 100 10 -1

            r XADD k * foo bar ; # replica is the primary now
            set res [$rd_xread read]
            set res [lindex [lindex [lindex [lindex $res 0] 1] 0] 1]
            assert_equal "foo bar" $res

            $rd_xread close
        }
    }
}
