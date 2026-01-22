set testmodule [file normalize tests/modules/aclcheck.so]

start_server {tags {"modules acl"}} {
    r module load $testmodule

    test {test module check acl for command perm} {
        # by default all commands allowed
        assert_equal [r aclcheck.rm_call.check.cmd set x 5] OK
        # block SET command for user
        r acl setuser default -set
        catch {r aclcheck.rm_call.check.cmd set x 5} e
        assert_match {*DENIED CMD*} $e

        # verify that new log entry added
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry username] eq {default}}
        assert {[dict get $entry context] eq {module}}
        assert {[dict get $entry object] eq {set}}
        assert {[dict get $entry reason] eq {command}}
    }

    test {test module check acl for key perm} {
        # give permission for SET and block all keys but x(READ+WRITE), y(WRITE), z(READ)
        r acl setuser default +set resetkeys ~x %W~y %R~z

        assert_equal [r aclcheck.set.check.key "*" x 5] OK
        catch {r aclcheck.set.check.key "*" v 5} e
        assert_match "*DENIED KEY*" $e

        assert_equal [r aclcheck.set.check.key "~" x 5] OK
        assert_equal [r aclcheck.set.check.key "~" y 5] OK
        assert_equal [r aclcheck.set.check.key "~" z 5] OK
        catch {r aclcheck.set.check.key "~" v 5} e
        assert_match "*DENIED KEY*" $e

        assert_equal [r aclcheck.set.check.key "W" y 5] OK
        catch {r aclcheck.set.check.key "W" v 5} e
        assert_match "*DENIED KEY*" $e

        assert_equal [r aclcheck.set.check.key "R" z 5] OK
        catch {r aclcheck.set.check.key "R" v 5} e
        assert_match "*DENIED KEY*" $e
    }

    test {test module check acl for module user} {
        # the module user has access to all keys
        assert_equal [r aclcheck.rm_call.check.cmd.module.user set y 5] OK
    }

    test {test module check acl for channel perm} {
        # block all channels but ch1
        r acl setuser default resetchannels &ch1
        assert_equal [r aclcheck.publish.check.channel ch1 msg] 0
        catch {r aclcheck.publish.check.channel ch2 msg} e
        set e
    } {*DENIED CHANNEL*}

    test {test module check acl in rm_call} {
        # rm call check for key permission (x: READ + WRITE)
        assert_equal [r aclcheck.rm_call set x 5] OK
        assert_equal [r aclcheck.rm_call set x 6 get] 5

        # rm call check for key permission (y: only WRITE)
        assert_equal [r aclcheck.rm_call set y 5] OK
        assert_error {*NOPERM*} {r aclcheck.rm_call set y 5 get}
        assert_error {*NOPERM*No permissions to access a key*} {r aclcheck.rm_call_with_errors set y 5 get}

        # rm call check for key permission (z: only READ)
        assert_error {*NOPERM*} {r aclcheck.rm_call set z 5}
        catch {r aclcheck.rm_call_with_errors set z 5} e
        assert_match {*NOPERM*No permissions to access a key*} $e
        assert_error {*NOPERM*} {r aclcheck.rm_call set z 6 get}
        assert_error {*NOPERM*No permissions to access a key*} {r aclcheck.rm_call_with_errors set z 6 get}

        # verify that new log entry added
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry username] eq {default}}
        assert {[dict get $entry context] eq {module}}
        assert {[dict get $entry object] eq {z}}
        assert {[dict get $entry reason] eq {key}}

        # rm call check for command permission
        r acl setuser default -set
        assert_error {*NOPERM*} {r aclcheck.rm_call set x 5}
        assert_error {*NOPERM*has no permissions to run the 'set' command*} {r aclcheck.rm_call_with_errors set x 5}

        # verify that new log entry added
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry username] eq {default}}
        assert {[dict get $entry context] eq {module}}
        assert {[dict get $entry object] eq {set}}
        assert {[dict get $entry reason] eq {command}}
    }

    test {test module comprehensive ACL check} {
        r acl setuser testuser on >testpass ~* &* +@all alldbs
        assert_equal [r auth testuser testpass] OK

        # Valid command with valid database
        assert_equal [r aclcheck.check.permissions 0 set x 5] OK
        assert_equal [r get x] 5

        # Denied command
        r acl setuser testuser -set
        catch {r aclcheck.check.permissions 0 set y 10} e
        assert_match {*NOPERM*} $e

        # Check ACL log entry
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry username] eq {testuser}}
        assert {[dict get $entry context] eq {module}}
        assert {[dict get $entry object] eq {set}}
        assert {[dict get $entry reason] eq {command}}
        r ACL LOG RESET

        # Denied key
        r acl setuser testuser +set resetkeys ~allowed_*
        assert_equal [r aclcheck.check.permissions 0 set allowed_key value] OK
        catch {r aclcheck.check.permissions 0 set denied_key value} e
        assert_match {*NOPERM*} $e
        
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry username] eq {testuser}}
        assert {[dict get $entry context] eq {module}}
        assert {[dict get $entry reason] eq {key}}
        r ACL LOG RESET

        # Denied channel
        r acl setuser testuser resetchannels &ch1
        assert_equal [r aclcheck.check.permissions 0 publish ch1 msg] 0
        catch {r aclcheck.check.permissions 0 publish ch2 msg} e
        assert_match {*NOPERM*} $e
        
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry username] eq {testuser}}
        assert {[dict get $entry context] eq {module}}
        assert {[dict get $entry reason] eq {channel}}
        r ACL LOG RESET

        # Denied db
        r acl setuser testuser allkeys resetdbs db=0,1,2
        assert_equal [r aclcheck.check.permissions 0 set testkey val] OK
        assert_equal [r aclcheck.check.permissions 1 set testkey val] OK
        catch {r aclcheck.check.permissions 3 set testkey val} e
        assert_match {*NOPERM*} $e
        
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry username] eq {testuser}}
        assert {[dict get $entry context] eq {module}}
        assert {[dict get $entry reason] eq {database}}
        r ACL LOG RESET

        # Invalid dbid
        catch {r aclcheck.check.permissions -1 set x 5} e
        assert_match {*invalid arguments*} $e

        catch {r aclcheck.check.permissions 999 set x 5} e
        assert_match {*invalid arguments*} $e

        # Invalid command
        catch {r aclcheck.check.permissions 0 nonexistentcmd arg1 arg2} e
        assert_match {*invalid arguments*} $e

        assert_equal [r auth default ""] OK
        r acl deluser testuser
        # Reset default user ACL to ensure clean state for next tests
        r acl setuser default on nopass ~* &* +@all alldbs
    }

    test {test blocking of Commands outside of OnLoad} {
        assert_equal [r block.commands.outside.onload] OK
    }

    test {test users to have access to module commands having acl categories} {
        r acl SETUSER j1 on >password -@all +@WRITE
        r acl SETUSER j2 on >password -@all +@READ
        assert_equal [r acl DRYRUN j1 aclcheck.module.command.aclcategories.write] OK
        assert_equal [r acl DRYRUN j2 aclcheck.module.command.aclcategories.write.function.read.category] OK
        assert_equal [r acl DRYRUN j2 aclcheck.module.command.aclcategories.read.only.category] OK
    }

    test {Unload the module - aclcheck} {
        assert_equal {OK} [r module unload aclcheck]
    }
}

start_server {tags {"modules check access for keys with prefix"}} {
    r module load $testmodule

    test {test module check acl check permissions for key prefix} {
        # Create a testuser that can only write to keys starting with "write_" prefix and read
        # from keys starting with "read_"
        assert_equal [r ACL SETUSER testuser on >1234 %W~write_* %R~read_* +@all -@dangerous] OK
        assert_equal [r AUTH testuser 1234] OK
        # No access
        assert_equal [r acl.check_key_prefix wr* W] EACCESS
        # We have write permissions for keys starting with write_*
        assert_equal [r acl.check_key_prefix write_* W] OK
        # write_1 matches the pattern write_* -> OK
        assert_equal [r acl.check_key_prefix write_1 W] OK
        # We can not read from write_* keys
        assert_equal [r acl.check_key_prefix write_1 R] EACCESS
        # But we can read from read_1 keys
        assert_equal [r acl.check_key_prefix read_1 R] OK
        # Missing the _
        assert_equal [r acl.check_key_prefix read* R] EACCESS 
        assert_equal [r acl.check_key_prefix read_* R] OK
    }
}

start_server {tags {"modules check access for keys with prefix"}} {
    r module load $testmodule

    test {test module check acl check permissions for key prefix (read=*)} {
        # Create a testuser that can only write to keys starting with "write_" prefix and read
        # from keys starting with "read_"
        assert_equal [r ACL SETUSER testuser on >1234 %W~write_* %R~* +@all -@dangerous] OK
        assert_equal [r AUTH testuser 1234] OK
        # No access
        assert_equal [r acl.check_key_prefix wr* W] EACCESS
        # We have write permissions for keys starting with write_*
        assert_equal [r acl.check_key_prefix write_* W] OK
        # write_1 matches the pattern write_* -> OK
        assert_equal [r acl.check_key_prefix write_1 W] OK
        # We can read from all keys ("*")
        assert_equal [r acl.check_key_prefix read_1 R] OK
        assert_equal [r acl.check_key_prefix read* R] OK
        assert_equal [r acl.check_key_prefix read_* R] OK
        assert_equal [r acl.check_key_prefix write_1 R] OK
    }
}

start_server {tags {"modules acl"}} {
    test {test existing users to have access to module commands loaded on runtime} {
        r acl SETUSER j3 on >password -@all +@WRITE
        assert_equal [r module load $testmodule] OK
        assert_equal [r acl DRYRUN j3 aclcheck.module.command.aclcategories.write] OK
        assert_equal {OK} [r module unload aclcheck]
    }
}

start_server {tags {"modules acl"}} {
    test {test existing users without permissions, do not have access to module commands loaded on runtime.} {
        r acl SETUSER j4 on >password -@all +@READ
        r acl SETUSER j5 on >password -@all +@WRITE
        assert_equal [r module load $testmodule] OK
        catch {r acl DRYRUN j4 aclcheck.module.command.aclcategories.write} e
        assert_equal {User j4 has no permissions to run the 'aclcheck.module.command.aclcategories.write' command} $e
        catch {r acl DRYRUN j5 aclcheck.module.command.aclcategories.write.function.read.category} e
        assert_equal {User j5 has no permissions to run the 'aclcheck.module.command.aclcategories.write.function.read.category' command} $e
    }

    test {test users without permissions, do not have access to module commands.} {
        r acl SETUSER j6 on >password -@all +@READ
        catch {r acl DRYRUN j6 aclcheck.module.command.aclcategories.write} e
        assert_equal {User j6 has no permissions to run the 'aclcheck.module.command.aclcategories.write' command} $e
        r acl SETUSER j7 on >password -@all +@WRITE
        catch {r acl DRYRUN j7 aclcheck.module.command.aclcategories.write.function.read.category} e
        assert_equal {User j7 has no permissions to run the 'aclcheck.module.command.aclcategories.write.function.read.category' command} $e
    }

    test {test if foocategory acl categories is added} {
        r acl SETUSER j8 on >password -@all +@foocategory
        assert_equal [r acl DRYRUN j8 aclcheck.module.command.test.add.new.aclcategories] OK
    }

    test {test if ACL CAT output for the new category is correct} {
        assert_equal [r ACL CAT foocategory] aclcheck.module.command.test.add.new.aclcategories
    }

    test {test permission compaction and simplification for categories added by a module} {
        r acl SETUSER j9 on >password -@all +@foocategory -@foocategory
        catch {r ACL GETUSER j9} res
        assert_equal {-@all -@foocategory} [lindex $res 5]
        assert_equal {OK} [r module unload aclcheck]
    }
}

start_server {tags {"modules acl"}} {
    test {test module load fails if exceeds the maximum number of adding acl categories} {
        assert_error {ERR Error loading the extension. Please check the server logs.} {r module load $testmodule 1}
    }
}
