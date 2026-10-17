source "../tests/includes/init-tests.tcl"

test "SENTINEL CONFIG SET and SENTINEL CONFIG GET handles multiple variables" {
    foreach_sentinel_id id {
        assert_equal {OK} [S $id SENTINEL CONFIG SET resolve-hostnames yes announce-port 1234]
    }
    assert_match {*yes*1234*} [S 1 SENTINEL CONFIG GET resolve-hostnames announce-port]
    assert_match {announce-port 1234} [S 1 SENTINEL CONFIG GET announce-port]
}

test "SENTINEL CONFIG GET for duplicate and unknown variables" {
    assert_equal {OK} [S 1 SENTINEL CONFIG SET resolve-hostnames yes announce-port 1234]
    assert_match {resolve-hostnames yes} [S 1 SENTINEL CONFIG GET resolve-hostnames resolve-hostnames does-not-exist]
}

test "SENTINEL CONFIG GET for patterns" {
    assert_equal {OK} [S 1 SENTINEL CONFIG SET loglevel notice announce-port 1234 announce-hostnames yes ]
    assert_match {loglevel notice} [S 1 SENTINEL CONFIG GET log* *level loglevel]
    assert_match {announce-hostnames yes announce-ip*announce-port 1234} [S 1 SENTINEL CONFIG GET announce*]
}

test "SENTINEL CONFIG GET aclfile" {
    assert_match {aclfile {}} [S 1 SENTINEL CONFIG GET aclfile]
    assert_match {*aclfile*} [S 1 SENTINEL CONFIG GET acl*]
    assert_match {*aclfile*} [S 1 SENTINEL CONFIG GET *file]

    # Restart Sentinel with a configured nonempty aclfile
    set sentinel_id 1
    set dirname "sentinel_$sentinel_id"
    set cfgfile [file join $dirname "sentinel.conf"]
    set cfgfile_bak [file join $dirname "sentinel.conf.bak"]
    set tmpfile [file join $dirname "sentinel.conf.tmp"]
    set aclfile [file normalize [file join $dirname "test.acl"]]

    set fp [open $aclfile w]
    puts $fp "user default on nopass ~* &* +@all"
    close $fp

    file copy -force $cfgfile $cfgfile_bak
    rename abort_sentinel_test abort_sentinel_test_orig
    proc abort_sentinel_test msg [format {
        catch {
            if {[file exists %s]} {
                file copy -force %s %s
                file delete -force %s
            }
            file delete -force %s
        }
        abort_sentinel_test_orig $msg
    } [list $cfgfile_bak] [list $cfgfile_bak] [list $cfgfile] [list $cfgfile_bak] [list $aclfile]]
    try {
        delete_lines_with_pattern $cfgfile $tmpfile {^user }
        set fp [open $cfgfile a]
        puts $fp "aclfile $aclfile"
        close $fp

        kill_instance sentinel $sentinel_id
        restart_instance sentinel $sentinel_id
        wait_for_condition 200 50 {
            [catch {S $sentinel_id PING}] == 0
        } else {
            fail "Sentinel $sentinel_id did not come back after restart"
        }

        assert_equal [list aclfile $aclfile] [S $sentinel_id SENTINEL CONFIG GET aclfile]
        assert_match "*aclfile [string map {* \\* ? \\? \[ \\[ \] \\]} $aclfile]*" [S $sentinel_id SENTINEL CONFIG GET acl*]
        assert_match "*aclfile [string map {* \\* ? \\? \[ \\[ \] \\]} $aclfile]*" [S $sentinel_id SENTINEL CONFIG GET *file]
    } finally {
        catch {
            rename abort_sentinel_test ""
            rename abort_sentinel_test_orig abort_sentinel_test
        }
        catch {kill_instance sentinel $sentinel_id}
        if {[file exists $cfgfile_bak]} {
            file copy -force $cfgfile_bak $cfgfile
            file delete -force $cfgfile_bak
        }
        file delete -force $aclfile
        catch {restart_instance sentinel $sentinel_id}
        catch {
            wait_for_condition 200 50 {
                [catch {S $sentinel_id PING}] == 0
            }
        }
    }
}

test "SENTINEL CONFIG SET aclfile is rejected" {
    catch {[S 1 SENTINEL CONFIG SET aclfile /tmp/test.acl]} e
    if {![string match "*Invalid argument*" $e]} {
        fail "Expected Invalid argument error when attempting to set aclfile"
    }
}

test "SENTINEL CONFIG SET duplicate variables" {
    catch {[S 1 SENTINEL CONFIG SET resolve-hostnames yes announce-port 1234 announce-port 100]} e
    if {![string match "*Duplicate argument*" $e]} {
        fail "Should give wrong arity error"
    }
}

test "SENTINEL CONFIG SET, one option does not exist" {
    foreach_sentinel_id id {
        assert_equal {OK} [S $id SENTINEL CONFIG SET announce-port 111]
        catch {[S $id SENTINEL CONFIG SET does-not-exist yes announce-port 1234]} e
        if {![string match "*Invalid argument*" $e]} {
            fail "Should give Invalid argument error"
        }
    }
    # The announce-port should not be set to 1234 as it was called with a wrong argument
    assert_match {*111*} [S 1 SENTINEL CONFIG GET announce-port]
}

test "SENTINEL CONFIG SET, one option with wrong value" {
    foreach_sentinel_id id {
        assert_equal {OK} [S $id SENTINEL CONFIG SET resolve-hostnames no]
        catch {[S $id SENTINEL CONFIG SET announce-port -1234 resolve-hostnames yes]} e
        if {![string match "*Invalid value*" $e]} {
            fail "Expected to return Invalid value error"
        }
    }
    # The resolve-hostnames should not be set to yes as it was called after an argument with an invalid value
    assert_match {*no*} [S 1 SENTINEL CONFIG GET resolve-hostnames]
}

test "SENTINEL CONFIG SET, wrong number of arguments" {
    catch {[S 1 SENTINEL CONFIG SET resolve-hostnames yes announce-port 1234 announce-ip]} e
    if {![string match "*Missing argument*" $e]} {
        fail "Expected to return Missing argument error"
    }
}

test "Undo SENTINEL CONFIG SET changes" {
    foreach_sentinel_id id {
        assert_equal {OK} [S $id SENTINEL CONFIG SET resolve-hostnames no announce-port 0]
    }
    assert_match {*no*0*} [S 1 SENTINEL CONFIG GET resolve-hostnames announce-port]
}
