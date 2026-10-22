set testmodule [file normalize tests/modules/blockedclient.so]

# rename-command hides a command from clients. A module is part of the server,
# so ValkeyModule_Call also accepts the original name, except from a script and
# in a replicated call.
tags {"modules external:skip"} {
    set renames [list rename-command "CONFIG VCONFIG" \
                      rename-command "SET VSET" \
                      rename-command "ECHO VECHO" \
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

        test {A replicated module call needs the current name of a renamed command} {
            assert_error {*unknown command*} {r do_rm_call_fire_and_forget set k v}
            assert_equal OK [r do_rm_call_fire_and_forget vset k v]
            assert_equal v [r get k]
        }
    }
}
