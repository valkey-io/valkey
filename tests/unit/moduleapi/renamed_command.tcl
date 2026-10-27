set testmodule [file normalize tests/modules/blockedclient.so]

# rename-command hides a command from clients. A module is part of the server,
# so ValkeyModule_Call also accepts the original name, except from a script.
# A renamed command is replicated under its current name.
tags {"modules external:skip"} {
    set renames [list rename-command "CONFIG VCONFIG" \
                      rename-command "SET VSET" \
                      rename-command "ECHO VECHO" \
                      rename-command "BLPOP VBLPOP" \
                      rename-command "LASTSAVE \"\""]
    start_server [list config_lines $renames] {
        r module load $testmodule

        test {Clients cannot use a renamed or removed command by its original name} {
            assert_error {*unknown command*} {r config get port}
            assert_error {*unknown command*} {r lastsave}
        }

        test {A renamed command still works for clients under its new name} {
            assert_equal [lindex [r vconfig get port] 1] [srv 0 port]
        }

        test {Module can call a renamed command by its original name} {
            assert_equal [lindex [r do_rm_call config get port] 1] [srv 0 port]
        }

        test {Module can call a renamed command by its new name} {
            assert_equal [lindex [r do_rm_call vconfig get port] 1] [srv 0 port]
        }

        test {Module can call a command removed with rename-command} {
            assert_morethan_equal [r do_rm_call lastsave] 0
        }

        test {Scripts cannot call a renamed or removed command by its original name} {
            assert_error {*command called from script*} {r eval "return server.call('echo', 'hi')" 0}
            assert_error {*command called from script*} {r eval "return server.call('lastsave')" 0}
            # also through a module command that the script runs
            assert_error {*unknown command*} {r eval "return server.call('do_rm_call', 'lastsave')" 0}
            assert_equal hi [r eval "return server.call('vecho', 'hi')" 0]
            assert_equal hi [r do_rm_call echo hi]
        }

        # A blocking call hands the arguments over to the blocked client, which must keep the renamed command name and
        # not free the caller's vector.
        foreach cmd {rm_call vm_call_argv} {
            test "A blocking renamed command called by its original name unblocks - $cmd" {
                set rd [valkey_deferring_client]
                $rd do_${cmd}_async blpop l 0
                wait_for_blocked_clients_count 1
                r lpush l a
                assert_equal [$rd read] {l a}
                wait_for_blocked_clients_count 0
                $rd close
            }

            test "A blocking renamed command called by its original name and replicated - $cmd" {
                assert_equal {Blocked} [r do_${cmd}_fire_and_forget blpop l 0]
                r lpush l a
                wait_for_blocked_clients_count 0
                assert_equal 0 [r llen l]
            }
        }

        test {A removed command cannot be called in a replicated call} {
            assert_error {*unknown command*} {r do_rm_call_fire_and_forget lastsave}
        }
    }

    # CONFIG stays available here, the replication stream helpers need it.
    start_server [list config_lines [list rename-command "SET VSET" appendonly yes]] {
        r module load $testmodule

        test {A replicated call by the original name of a renamed command is propagated under its current name} {
            set repl [attach_to_replication_stream]
            assert_equal OK [r do_rm_call_fire_and_forget set k v]
            assert_replication_stream $repl {
                {select *}
                {vset k v}
            }
            close_replication_stream $repl
            assert_equal v [r get k]
        }

        test {A write replicated by the original name of a renamed command survives an AOF reload} {
            assert_equal OK [r do_rm_call_fire_and_forget set k2 v2]
            r debug loadaof
            assert_equal v2 [r get k2]
        }
    }
}
