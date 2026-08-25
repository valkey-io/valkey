start_server {tags {needs:repl external:skip}} {
    start_server {} {
        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        set primary_pid [srv -1 pid]

        set replica_host [srv 0 host]
        set replica_port [srv 0 port]
        set replica_pid [srv 0 pid]

        test {write command inside MULTI is QUEUED, EXEC should be REDIRECT} {
            set rr [valkey_client]
            $rr client capa redirect
            $rr multi
            assert_equal "QUEUED" [$rr set foo bar]

            # Change the current instance to be a replica
            r replicaof $primary_host $primary_port
            wait_replica_online $primary

            # In case of bio thread RDB download, there can be up to 1000ms 
            # (1 replication cron loop) delay until the rdb starts loading
            after 1000 
            wait_done_loading r

            assert_error "REDIRECT*" {$rr exec}
            $rr close
        }

        test {write command inside MULTI is REDIRECT, EXEC should be EXECABORT} {
            set rr [valkey_client]
            $rr client capa redirect
            $rr multi
            assert_error "REDIRECT*" {$rr set foo bar}
            assert_error "EXECABORT*" {$rr exec}
            $rr close
        }

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

        test {CLIENT INFO} {
            r client info
        } {id=* addr=*:* laddr=*:* fd=* name=* age=* idle=* flags=N capa=r db=* sub=0 psub=0 ssub=0 multi=-1 watch=0 qbuf=0 qbuf-free=* argv-mem=* multi-mem=0 rbs=* rbp=* obl=0 oll=0 omem=0 tot-mem=* events=r cmd=client|info user=* redir=-1 resp=* lib-name=* lib-ver=* tot-net-in=* tot-net-out=* tot-cmds=*}

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

        test {blocked clients behavior during failover} {
            r -1 replicaof no one
            r -1 flushall
            r replicaof $primary_host $primary_port
            wait_replica_online $primary

            # Client blocking on primary
            set rd0 [valkey_deferring_client -1]
            $rd0 CLIENT CAPA REDIRECT
            assert_match "OK" [$rd0 read]
            $rd0 BLPOP mylist 0

            # Readonly client blocking on primary
            set rd0_ro [valkey_deferring_client -1]
            $rd0_ro CLIENT CAPA REDIRECT
            assert_match "OK" [$rd0_ro read]
            $rd0_ro READONLY
            assert_match "OK" [$rd0_ro read]
            $rd0_ro XREAD BLOCK 0 STREAMS mystream 0-0

            # Readonly client blocking on replica
            set rd1 [valkey_deferring_client 0]
            $rd1 CLIENT CAPA REDIRECT
            assert_match "OK" [$rd1 read]
            $rd1 READONLY
            assert_equal OK [$rd1 read]
            $rd1 XREAD BLOCK 0 STREAMS k 0-0

            wait_for_condition 50 100 {
                [s -1 blocked_clients] eq 2 &&
                [s 0 blocked_clients] eq 1
            } else {
                fail "client wasn't blocked"
            }

            r -1 FAILOVER TO $replica_host $replica_port

            wait_for_condition 50 100 {
                [s -1 role] eq {slave} &&
                [s 0 role] eq {master}
            } else {
                fail "Failover did not complete"
            }

            assert_error "REDIRECT $replica_host:$replica_port" {$rd0 read}

            # Check that the readonly client blocked on the new replica (old primary) is still blocked.
            assert_equal 1 [s 0 blocked_clients]

            # Check that the client blocked on the new primary (old replica) is still blocked.
            assert_equal 1 [s -1 blocked_clients]

            # Add an entry to the stream to unblock the blocking XREAD.
            set stream_id [r XADD k * foo bar]
            assert_equal "{k {{$stream_id {foo bar}}}}" [$rd1 read]
            set stream_id [r XADD mystream * foo bar]
            assert_equal "{mystream {{$stream_id {foo bar}}}}" [$rd0_ro read]

            $rd0 close
            $rd0_ro close
            $rd1 close
        }
    }
}

