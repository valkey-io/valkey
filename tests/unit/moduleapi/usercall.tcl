set testmodule [file normalize tests/modules/usercall.so]

set test_script_set "#!lua
redis.call('set','x',1)
return 1"

set test_script_get "#!lua
redis.call('get','x')
return 1"

start_server {tags {"modules usercall network"}} {
    r module load $testmodule

    # baseline test that module isn't doing anything weird
    foreach cmd {call_without_user call_argv_without_user} {
        test "test module check regular valkey command without user/acl with $cmd" {
            assert_equal [r usercall.reset_user] OK
            assert_equal [r usercall.add_to_acl "~* &* +@all -set"] OK
            assert_equal [r usercall.$cmd set x 5] OK
            assert_equal [r usercall.reset_user] OK
        }
    }

    # call with user with acl set on it, but without testing the acl
    foreach cmd {call_with_user_flag call_argv_with_user_flag} {
        test "test module check regular valkey command with user with $cmd" {
            assert_equal [r set x 5] OK

            assert_equal [r usercall.reset_user] OK
            assert_equal [r usercall.add_to_acl "~* &* +@all -set"] OK
            # off because module user / default value
            assert_equal [r usercall.get_acl] "off ~* &* +@all -set"

            # doesn't fail for regular commands as just testing acl here
            assert_equal [r usercall.$cmd {} set x 10] OK

            assert_equal [r get x] 10
            assert_equal [r usercall.reset_user] OK
        }
    }

    # call with user with acl set on it, but with testing the acl in rm_call (for cmd itself)
    foreach cmd {call_with_user_flag call_argv_with_user_flag} {
        test "test module check regular valkey command with user and acl with $cmd" {
            assert_equal [r set x 5] OK

            r ACL LOG RESET
            assert_equal [r usercall.reset_user] OK
            assert_equal [r usercall.add_to_acl "~* &* +@all -set"] OK
            # off because module user / default value
            assert_equal [r usercall.get_acl] "off ~* &* +@all -set"

            # fails here as testing acl in rm call
            assert_error {*NOPERM User module_user has no permissions*} {r usercall.$cmd C set x 10}

            assert_equal [r usercall.$cmd C get x] 5

            # verify that new log entry added
            set entry [lindex [r ACL LOG] 0]
            assert_equal [dict get $entry username] {module_user}
            assert_equal [dict get $entry context] {module}
            assert_equal [dict get $entry object] {set}
            assert_equal [dict get $entry reason] {command}
            assert_match "*cmd=usercall.$cmd*" [dict get $entry client-info]

            assert_equal [r usercall.reset_user] OK
        }
    }

    # call with user with acl set on it, but with testing the acl in rm_call (for cmd itself)
    test {test module check regular valkey command with user and acl from blocked background thread} {
        assert_equal [r set x 5] OK

        r ACL LOG RESET
        assert_equal [r usercall.reset_user] OK
        assert_equal [r usercall.add_to_acl "~* &* +@all -set"] OK

        # fails here as testing acl in rm call from a background thread
        assert_error {*NOPERM User module_user has no permissions*} {r usercall.call_with_user_bg C set x 10}

        assert_equal [r usercall.call_with_user_bg C get x] 5

        # verify that new log entry added
        set entry [lindex [r ACL LOG] 0]
        assert_equal [dict get $entry username] {module_user}
        assert_equal [dict get $entry context] {module}
        assert_equal [dict get $entry object] {set}
        assert_equal [dict get $entry reason] {command}
        assert_match {*cmd=NULL*} [dict get $entry client-info]

        assert_equal [r usercall.reset_user] OK
    }

    # baseline script test, call without user on script
    foreach cmd {call_without_user call_argv_without_user} {
        test "test module check eval script without user with $cmd" {
            set sha_set [r script load $test_script_set]
            set sha_get [r script load $test_script_get]

            assert_equal [r usercall.$cmd evalsha $sha_set 0] 1
            assert_equal [r usercall.$cmd evalsha $sha_get 0] 1
        }
    }

    # baseline script test, call without user on script
    foreach cmd {call_with_user_flag call_argv_with_user_flag} {
        test "test module check eval script with user being set, but not acl testing with $cmd" {
            set sha_set [r script load $test_script_set]
            set sha_get [r script load $test_script_get]

            assert_equal [r usercall.reset_user] OK
            assert_equal [r usercall.add_to_acl "~* &* +@all -set"] OK
            # off because module user / default value
            assert_equal [r usercall.get_acl] "off ~* &* +@all -set"

            # passes as not checking ACL
            assert_equal [r usercall.$cmd {} evalsha $sha_set 0] 1
            assert_equal [r usercall.$cmd {} evalsha $sha_get 0] 1
        }
    }

    # call with user on script (without rm_call acl check) to ensure user carries through to script execution
    # we already tested the check in rm_call above, here we are checking the script itself will enforce ACL
    foreach cmd {call_with_user_flag call_argv_with_user_flag} {
        test "test module check eval script with user and acl with $cmd" {
            set sha_set [r script load $test_script_set]
            set sha_get [r script load $test_script_get]

            r ACL LOG RESET
            assert_equal [r usercall.reset_user] OK
            assert_equal [r usercall.add_to_acl "~* &* +@all -set"] OK

            # fails here in script, as rm_call will permit the eval call
            catch {r usercall.$cmd C evalsha $sha_set 0} e
            assert_match {*ERR ACL failure in script*} $e

            assert_equal [r usercall.$cmd C evalsha $sha_get 0] 1

            # verify that new log entry added
            set entry [lindex [r ACL LOG] 0]
            assert_equal [dict get $entry username] {module_user}
            assert_equal [dict get $entry context] {lua}
            assert_equal [dict get $entry object] {set}
            assert_equal [dict get $entry reason] {command}
            assert_match "*cmd=usercall.$cmd*" [dict get $entry client-info]
        }
    }

    # The commandlog attributes a command to the user it ran as. When a script
    # reaches a module command that selects its own user (SetContextUser plus the
    # 'C' flag), the inner call must be logged as that user, not as the user of
    # the connection that started the script.
    foreach cmd {call_with_user_flag call_argv_with_user_flag} {
        test "test module commandlog reports the executing user with $cmd" {
            set sha [r script load "#!lua
server.call('usercall.$cmd','C','get','x')
return 1"]

            assert_equal [r usercall.reset_user] OK
            assert_equal [r usercall.add_to_acl "~* &* +@all"] OK

            r config set commandlog-execution-slower-than 0
            r commandlog reset slow
            assert_equal [r evalsha $sha 0] 1
            r config set commandlog-execution-slower-than -1

            # Newest first: the script, the module command, then the GET the module
            # ran on behalf of its own user.
            set entries [r commandlog get -1 slow]
            assert_equal {evalsha} [lindex [lindex $entries 0] 3 0]
            assert_equal "usercall.$cmd C get x" [lindex [lindex $entries 1] 3]
            assert_equal {get x} [lindex [lindex $entries 2] 3]

            assert_equal {module_user} [lindex [lindex $entries 2] 6]
            # The script and the module command itself ran as the connection's user.
            assert_equal {default} [lindex [lindex $entries 0] 6]
            assert_equal {default} [lindex [lindex $entries 1] 6]
        }
    }

    start_server {tags {"wait aof external:skip"}} {
        set slave [srv 0 client]
        set slave_host [srv 0 host]
        set slave_port [srv 0 port]
        set slave_pid [srv 0 pid]
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]

        $master config set appendonly yes
        $master config set appendfsync everysec
        $slave config set appendonly yes
        $slave config set appendfsync everysec

        test {Setup slave} {
            $slave slaveof $master_host $master_port
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }
        }

        foreach cmd {call_with_user_flag call_argv_with_user_flag} {
            test "test module replicate only to replicas and WAITAOF with $cmd" {
                $master set x 1
                assert_equal [$master waitaof 1 1 10000] {1 1}
                $master usercall.$cmd A! config set loglevel notice
                # Make sure WAITAOF doesn't hang
                assert_equal [$master waitaof 1 1 10000] {1 1}
            }
        }
    }
}
