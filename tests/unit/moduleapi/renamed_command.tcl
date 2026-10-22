set testmodule [file normalize tests/modules/blockedclient.so]

# rename-command hides a command from clients. A module is part of the server,
# so ValkeyModule_Call resolves commands by their original name regardless.
tags {"modules external:skip"} {
    set renames [list rename-command "CONFIG VCONFIG" \
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
    }
}
