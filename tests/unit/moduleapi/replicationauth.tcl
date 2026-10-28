set testmodule [file normalize tests/modules/replicationauth.so]
set secondmodule [file normalize [file join [tmpdir replicationauth] replicationauthtwo.so]]
file copy -force $testmodule $secondmodule

foreach dualchannel {no yes} {
    start_server {tags {"modules repl network external:skip cluster:skip"} overrides {save ""}} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        $primary config set repl-diskless-sync yes repl-diskless-sync-delay 0
        $primary config set dual-channel-replication-enabled $dualchannel
        $primary acl setuser default resetpass >default-token
        $primary acl setuser token-user on >token-one +ping +replconf +psync +sync
        $primary acl setuser static-user on >static-token +ping +replconf +psync +sync

        start_server {overrides {save ""}} {
            set replica [srv 0 client]
            $replica config set dual-channel-replication-enabled $dualchannel

            test "Replication credential callback registration, dual channel: $dualchannel" {
                assert_equal OK [$replica module load $testmodule]
                assert_equal OK [$replica replicationauth.set token-user token-one]
                assert_equal 1 [$replica replicationauth.register]
            }

            test "Module credentials work without primaryauth, dual channel: $dualchannel" {
                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica
                $primary set credential-test first
                wait_for_ofs_sync $primary $replica
                assert_equal first [$replica get credential-test]
                assert_morethan [$replica replicationauth.calls] [expr {$dualchannel eq "yes" ? 1 : 0}]
            }

            test "Credential rotation affects new connections, dual channel: $dualchannel" {
                $primary acl setuser token-user resetpass >token-two
                $replica replicationauth.set token-user token-two
                assert_equal up [status $replica master_link_status]
                $replica replicaof no one
                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica
                $primary set credential-test rotated
                wait_for_ofs_sync $primary $replica
                assert_equal rotated [$replica get credential-test]
                assert_equal {} [lindex [$replica config get primaryauth] 1]
            }

            test "Credentials preserve binary passwords, dual channel: $dualchannel" {
                set binary_token "token\x00with\r\nbytes"
                $primary acl setuser token-user resetpass >$binary_token
                $replica replicaof no one
                $replica replicationauth.set token-user $binary_token
                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica
                $primary set credential-test binary-password
                wait_for_ofs_sync $primary $replica
                assert_equal binary-password [$replica get credential-test]
                $primary acl setuser token-user resetpass >token-two
                $replica replicationauth.set token-user token-two
            }

            test "Provider errors never use static credentials, dual channel: $dualchannel" {
                $replica replicaof no one
                $replica config set primaryuser static-user primaryauth static-token
                $replica replicationauth.mode error
                set calls [$replica replicationauth.calls]
                set loglines [count_log_lines 0]
                $replica replicaof $primary_host $primary_port
                wait_for_condition 50 100 {
                    [$replica replicationauth.calls] > $calls
                } else { fail "replication credentials callback was not called" }
                wait_for_log_messages 0 {"*Replication credentials callback failed*"} $loglines 50 100
                assert_equal down [status $replica master_link_status]
                $replica replicationauth.mode ok
                wait_for_sync $replica
            }

            test "Missing provider password fails closed, dual channel: $dualchannel" {
                $replica replicaof no one
                $replica replicationauth.mode missing-password
                set calls [$replica replicationauth.calls]
                set loglines [count_log_lines 0]
                $replica replicaof $primary_host $primary_port
                wait_for_condition 50 100 {
                    [$replica replicationauth.calls] > $calls
                } else { fail "replication credentials callback was not called" }
                wait_for_log_messages 0 {"*Replication credentials callback failed*"} $loglines 50 100
                assert_equal down [status $replica master_link_status]
                $replica replicationauth.mode ok
                wait_for_sync $replica
            }

            if {$dualchannel eq "yes"} {
                test {RDB channel invokes the provider and recovers after callback failure} {
                    $replica replicaof no one
                    $replica replicationauth.mode error-after-first
                    set calls [$replica replicationauth.calls]
                    set loglines [count_log_lines 0]
                    $replica replicaof $primary_host $primary_port
                    wait_for_condition 50 100 {
                        [$replica replicationauth.calls] >= $calls + 2
                    } else { fail "RDB channel did not invoke the provider" }
                    wait_for_log_messages 0 {"*dual channel replication handshake*Replication credentials callback failed*"} $loglines 50 100
                    assert_equal down [status $replica master_link_status]
                    $replica replicationauth.mode ok
                    wait_for_sync $replica
                }
            }

            test "Changing replication state in the callback cancels the handshake, dual channel: $dualchannel" {
                $replica replicaof no one
                if {$dualchannel eq "yes"} {
                    $primary debug change-repl-id
                    $primary debug change-repl-id
                }
                $replica replicationauth.mode [expr {$dualchannel eq "yes" ? "cancel-after-first" : "cancel-handshake"}]
                set calls [$replica replicationauth.calls]
                $replica replicaof $primary_host $primary_port
                wait_for_condition 50 100 {
                    [$replica replicationauth.calls] > $calls && [status $replica role] eq "master"
                } else { fail "callback did not cancel replication: calls $calls to [$replica replicationauth.calls], role [status $replica role]" }
                after 1200
                assert_equal master [status $replica role]
                assert_equal OK [$replica replicationauth.mode ok]
                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica
            }

            test "Callback AutoMemory strings survive context cleanup, dual channel: $dualchannel" {
                $replica replicaof no one
                $replica replicationauth.mode auto-memory
                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica
                $primary set credential-test auto-memory
                wait_for_ofs_sync $primary $replica
                assert_equal auto-memory [$replica get credential-test]
            }

            test "Credential callback cannot unload itself, dual channel: $dualchannel" {
                $replica replicaof no one
                $replica replicationauth.mode self-unload
                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica
                assert_equal OK [$replica replicationauth.mode ok]
            }

            test "Credentials support an omitted username, dual channel: $dualchannel" {
                $replica replicaof no one
                $replica replicationauth.set - default-token
                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica
                $primary set credential-test default-user
                wait_for_ofs_sync $primary $replica
                assert_equal default-user [$replica get credential-test]
                $replica replicaof no one
                $replica replicationauth.set token-user token-two
            }

            test "Latest loaded provider wins and unload restores the prior one, dual channel: $dualchannel" {
                assert_equal OK [$replica module load $secondmodule replicationauthtwo]
                $replica replicationauthtwo.set token-user wrong-token
                set calls [$replica replicationauth.calls]
                set loglines [count_log_lines 0]
                $replica replicaof $primary_host $primary_port
                wait_for_condition 50 100 {
                    [$replica replicationauthtwo.calls] > 0
                } else { fail "latest provider was not called" }
                wait_for_log_messages 0 {"*Unable to AUTH to PRIMARY:*WRONGPASS*"} $loglines 50 100
                assert_equal $calls [$replica replicationauth.calls]
                assert_equal down [status $replica master_link_status]
                assert_equal OK [$replica module unload replicationauthtwo]
                wait_for_sync $replica
            }

            test "Failed provider load leaves the previous provider active, dual channel: $dualchannel" {
                $replica replicaof no one
                assert_error {*initialization failed*} {
                    $replica module load $secondmodule failedprovider fail
                }
                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica
            }

            test "Unloading the last provider restores static auth, dual channel: $dualchannel" {
                $replica replicaof no one
                assert_equal OK [$replica module unload replicationauth]
                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica
                $primary set credential-test static
                wait_for_ofs_sync $primary $replica
                assert_equal static [$replica get credential-test]
            }
        }
    }
}
