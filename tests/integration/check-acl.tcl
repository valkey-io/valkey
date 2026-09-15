tags {"check-acl external:skip logreqres:skip"} {

    # Helper to write a temp ACL file and return its path
    proc write_acl_file {content} {
        set path [tmpfile "check-acl-test"]
        set fd [open $path w]
        puts -nonewline $fd $content
        close $fd
        return $path
    }

    test {check-acl: valid ACL file passes} {
        set path [write_acl_file "user alice on >password ~* +@all\nuser bob on >secret ~cache:* +get +set\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: invalid category is detected} {
        set path [write_acl_file "user alice on >pass +@boguscategory\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN $path} result
        assert_match "*Unknown command or category name in ACL*" $result
    }

    test {check-acl: invalid command is detected} {
        set path [write_acl_file "user alice on >pass +nonexistentcommand\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN $path} result
        assert_match "*Unknown command or category name in ACL*" $result
    }

    test {check-acl: bad password hash is detected} {
        set path [write_acl_file "user alice on #badhash\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN $path} result
        assert_match "*password hash must be exactly 64 characters*" $result
    }

    test {check-acl: missing user keyword is detected} {
        set path [write_acl_file "alice on >pass ~* +@all\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN $path} result
        assert_match "*should start with*user*" $result
    }

    test {check-acl: unbalanced quotes detected} {
        set path [write_acl_file "user alice on >pass \"unbalanced\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN $path} result
        assert_match "*Unbalanced quotes*" $result
    }

    test {check-acl: multiple errors collected} {
        set path [write_acl_file "user alice on +@bad1\nuser bob on +@bad2\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN $path} result
        assert_match "*bad1*" $result
        assert_match "*bad2*" $result
    }

    test {check-acl: --fail-fast stops at first error} {
        set path [write_acl_file "user alice on +@bad1\nuser bob on +@bad2\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --fail-fast $path} result
        assert_match "*bad1*" $result
        assert_no_match "*bad2*" $result
    }

    test {check-acl: empty file is valid} {
        set path [write_acl_file ""]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: comments and blank lines are skipped} {
        set path [write_acl_file "# This is a comment\n\nuser alice on >pass ~* +@all\n# Another comment\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: valkey.conf mode skips non-user lines} {
        set path [write_acl_file "bind 127.0.0.1\nport 6379\nuser admin on >secret ~* +@all\ntimeout 0\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: TAB after user keyword is accepted} {
        set path [write_acl_file "user\talice on >pass ~* +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: selectors are validated} {
        set path [write_acl_file "user alice on >pass (~cache:* +get +set) (~logs:* +get)\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: unmatched parenthesis in selector} {
        set path [write_acl_file "user alice on >pass (~cache:* +get\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN $path} result
        assert_match "*Unmatched parenthesis*" $result
    }

    test {check-acl: --json output format} {
        set path [write_acl_file "user alice on +@bogus\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --json $path} result
        assert_match {*"valid":false*} $result
        assert_match {*"errors":*} $result
    }

    test {check-acl: --json valid file} {
        set path [write_acl_file "user alice on >pass ~* +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --json $path]
        assert_match {*"valid":true*} $result
    }

    test {check-acl: --level full warns about no-password user} {
        set path [write_acl_file "user alice on ~* +@all\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --level full $path} result
        assert_match "*no passwords*nopass*" $result
    }

    test {check-acl: --level full warns about overly permissive user} {
        set path [write_acl_file "user alice on >pass ~* +@all\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --level full $path} result
        assert_match "*full access*" $result
    }

    test {check-acl: --level full warns about missing default user} {
        set path [write_acl_file "user alice on >pass ~* +@all\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --level full $path} result
        assert_match "*No*default*user defined*" $result
    }

    test {check-acl: --level full no warnings for well-configured file} {
        set path [write_acl_file "user default on >pass ~* +@all\nuser readonly on >pass ~* +@read\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --level full $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: valid password hash accepted} {
        set path [write_acl_file "user alice on #0000000000000000000000000000000000000000000000000000000000000000 ~* +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: key patterns validated} {
        set path [write_acl_file "user alice on >pass ~cache:* %R~secret:* +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: channel patterns validated} {
        set path [write_acl_file "user alice on >pass &news:* +@all ~*\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: nopass user accepted} {
        set path [write_acl_file "user alice on nopass ~* +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: stdin input with dash} {
        set result [exec echo "user alice on >pass ~* +@all" | $::VALKEY_CHECK_ACL_BIN -]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: nonexistent file reports error} {
        catch {exec $::VALKEY_CHECK_ACL_BIN /nonexistent/path/acl.conf} result
        assert_match "*Error opening*" $result
    }

    test {check-acl: --help shows usage} {
        catch {exec $::VALKEY_CHECK_ACL_BIN --help} result
        assert_match "*Usage*valkey-check-acl*" $result
    }

    # ------------------------------------------------------------------
    # --version flag: version-gated syntax validation
    # ------------------------------------------------------------------

    test {check-acl: --version 6.0 accepts baseline ACL syntax} {
        set path [write_acl_file "user alice on >pass ~* +@all\nuser bob off resetpass resetkeys nocommands\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --version 6.0 $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: --version 6.0 rejects channel pattern &} {
        set path [write_acl_file "user alice on >pass &chat:* ~* +@all\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.0 $path} result
        assert_match "*Channel pattern*requires version >= 6.2*" $result
    }

    test {check-acl: --version 6.0 rejects allchannels} {
        set path [write_acl_file "user alice on >pass allchannels ~* +@all\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.0 $path} result
        assert_match "*allchannels*requires version >= 6.2*" $result
    }

    test {check-acl: --version 6.0 rejects resetchannels} {
        set path [write_acl_file "user alice on >pass resetchannels ~* +@all\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.0 $path} result
        assert_match "*resetchannels*requires version >= 6.2*" $result
    }

    test {check-acl: --version 6.2 accepts channel patterns} {
        set path [write_acl_file "user alice on >pass &news:* allchannels resetchannels &chat:* ~* +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --version 6.2 $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: --version 6.2 rejects selectors} {
        set path [write_acl_file "user alice on >pass ~* +get (+@write ~data:*)\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.2 $path} result
        assert_match "*Selectors*require version >= 7.0*" $result
    }

    test {check-acl: --version 6.2 rejects %R~ key pattern} {
        set path [write_acl_file "user alice on >pass %R~keys:* +@read\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.2 $path} result
        assert_match "*Key permission*requires version >= 7.0*" $result
    }

    test {check-acl: --version 6.2 rejects %W~ key pattern} {
        set path [write_acl_file "user alice on >pass %W~keys:* +@write\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.2 $path} result
        assert_match "*Key permission*requires version >= 7.0*" $result
    }

    test {check-acl: --version 6.2 rejects %RW~ key pattern} {
        set path [write_acl_file "user alice on >pass %RW~keys:* +@all\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.2 $path} result
        assert_match "*Key permission*requires version >= 7.0*" $result
    }

    test {check-acl: --version 6.2 rejects clearselectors} {
        set path [write_acl_file "user alice on >pass clearselectors ~* +@all\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.2 $path} result
        assert_match "*clearselectors*requires version >= 7.0*" $result
    }

    test {check-acl: --version 6.2 rejects sanitize-payload} {
        set path [write_acl_file "user alice on >pass sanitize-payload ~* +@all\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.2 $path} result
        assert_match "*sanitize-payload*requires version >= 7.0*" $result
    }

    test {check-acl: --version 6.2 rejects skip-sanitize-payload} {
        set path [write_acl_file "user alice on >pass skip-sanitize-payload ~* +@all\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.2 $path} result
        assert_match "*skip-sanitize-payload*requires version >= 7.0*" $result
    }

    test {check-acl: --version 6.2 rejects deny subcommand} {
        set path [write_acl_file "user alice on >pass ~* +@all -client|kill\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.2 $path} result
        assert_match "*Denying subcommands*requires version >= 7.0*" $result
    }

    test {check-acl: --version 7.0 accepts selectors and key permissions} {
        set path [write_acl_file "user alice on >pass %R~secret:* clearselectors (+@read ~cache:*) sanitize-payload +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --version 7.0 $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: --version 7.0 accepts deny subcommand} {
        set path [write_acl_file "user alice on >pass ~* +@all -client|kill\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --version 7.0 $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: --version 7.0 rejects alldbs} {
        set path [write_acl_file "user alice on >pass ~* +@all alldbs\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 7.0 $path} result
        assert_match "*alldbs*requires version >= 9.1*" $result
    }

    test {check-acl: --version 7.0 rejects resetdbs} {
        set path [write_acl_file "user alice on >pass ~* +@all resetdbs\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 7.0 $path} result
        assert_match "*resetdbs*requires version >= 9.1*" $result
    }

    test {check-acl: --version 7.0 rejects db=} {
        set path [write_acl_file "user alice on >pass ~* +@all db=0,1\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 7.0 $path} result
        assert_match "*db=*requires version >= 9.1*" $result
    }

    test {check-acl: --version 9.1 accepts database ACL} {
        set path [write_acl_file "user alice on >pass ~* +@all alldbs\nuser bob on >pass ~* +@all resetdbs db=0,1,2\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --version 9.1 $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: --version without flag accepts all current syntax} {
        set path [write_acl_file "user alice on >pass %R~secret:* &* alldbs (+@write ~data:*) sanitize-payload +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: --version with patch number works} {
        set path [write_acl_file "user alice on >pass &chat:* ~* +@all\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.0.0 $path} result
        assert_match "*Channel pattern*requires version >= 6.2*" $result
    }

    test {check-acl: --version invalid format rejected} {
        catch {exec $::VALKEY_CHECK_ACL_BIN --version abc /dev/null} result
        assert_match "*Invalid version format*" $result
    }

    test {check-acl: --version below minimum rejected} {
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 5.0 /dev/null} result
        assert_match "*Minimum supported version is 6.0*" $result
    }

    test {check-acl: --version with --json includes version field} {
        set path [write_acl_file "user alice on >pass &* ~* +@all\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.0 --json $path} result
        assert_match {*"version":"6.0.0"*} $result
        assert_match {*"valid":false*} $result
    }

    test {check-acl: --version 7.2 same as 7.0 syntax-wise} {
        set path [write_acl_file "user alice on >pass %R~data:* (+@read ~cache:*) +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --version 7.2 $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: --version 8.0 same as 7.0 syntax-wise} {
        set path [write_acl_file "user alice on >pass ~* (+@read ~cache:*) clearselectors sanitize-payload +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --version 8.0 $path]
        assert_match "*ACL file is valid*" $result
    }

    # ------------------------------------------------------------------
    # Additional edge cases
    # ------------------------------------------------------------------

    test {check-acl: multiple users with mixed valid and invalid} {
        set path [write_acl_file "user alice on >pass ~* +@all\nuser bob on >pass +@fakecategory\nuser carol on >pass ~* +get\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN $path} result
        assert_match "*fakecategory*" $result
        # Only 1 error, other users are fine
        assert_match "*1 error*" $result
    }

    test {check-acl: user with all password types} {
        set path [write_acl_file "user alice on >plainpass #0000000000000000000000000000000000000000000000000000000000000000 ~* +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: user with resetpass and nopass flags} {
        set path [write_acl_file "user alice on resetpass nopass ~* +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: user with allkeys and allcommands aliases} {
        set path [write_acl_file "user alice on >pass allkeys allcommands\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: user with nocommands alias} {
        set path [write_acl_file "user alice off nocommands\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: user with reset keyword} {
        set path [write_acl_file "user alice reset on >newpass ~* +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: user with multiple key patterns} {
        set path [write_acl_file "user alice on >pass ~cache:* ~session:* ~user:* +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: user with multiple selectors} {
        set path [write_acl_file "user alice on >pass ~default:* +get (+@write ~data:*) (+@read ~logs:*)\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: user with subcommand allow} {
        set path [write_acl_file "user alice on >pass ~* +config|get +client|info\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: --quiet suppresses valid message} {
        set path [write_acl_file "user alice on >pass ~* +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --quiet $path]
        assert_equal "" [string trim $result]
    }

    test {check-acl: --level syntax accepts unknown commands} {
        # At syntax level, we don't validate command names
        # But ACLStringSetUser still validates them - this tests current behavior
        set path [write_acl_file "user alice on >pass ~* +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --level syntax $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: exit code 0 for valid file} {
        set path [write_acl_file "user default on >pass ~* +@all\n"]
        exec $::VALKEY_CHECK_ACL_BIN $path
        # If we get here without catch, exit code was 0
    }

    test {check-acl: exit code 1 for errors} {
        set path [write_acl_file "user alice on +@bogus\n"]
        set code [catch {exec $::VALKEY_CHECK_ACL_BIN $path} result]
        assert_equal 1 $code
    }

    test {check-acl: exit code 3 for warnings only} {
        set path [write_acl_file "user alice on >pass ~* +@all\n"]
        set code [catch {exec $::VALKEY_CHECK_ACL_BIN --level full $path} result]
        # Warnings: overly permissive + missing default
        assert_equal 1 $code
    }

    test {check-acl: --version combined with --level full} {
        set path [write_acl_file "user alice on ~* +@all\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 7.0 --level full $path} result
        assert_match "*no passwords*nopass*" $result
    }

    test {check-acl: --version combined with --fail-fast} {
        set path [write_acl_file "user alice on >pass &foo ~* +@all\nuser bob on >pass &bar ~* +@all\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.0 --fail-fast $path} result
        assert_match "*Channel pattern*" $result
        # Should only report first error
        assert_no_match "*:2:*" $result
    }

    test {check-acl: large file with many users} {
        set content ""
        for {set i 0} {$i < 100} {incr i} {
            append content "user user$i on >pass$i ~prefix$i:* +@all\n"
        }
        set path [write_acl_file $content]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: file with only comments} {
        set path [write_acl_file "# comment 1\n# comment 2\n# comment 3\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: disabled user is valid} {
        set path [write_acl_file "user alice off\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: user with only username} {
        set path [write_acl_file "user alice\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN $path]
        assert_match "*ACL file is valid*" $result
    }

    # ------------------------------------------------------------------
    # Command version gating (cmd->since check)
    # ------------------------------------------------------------------

    test {check-acl: --version rejects command introduced later (LMPOP since 7.0)} {
        set path [write_acl_file "user alice on >pass ~* +lmpop\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.2 $path} result
        assert_match "*Command 'lmpop' requires version >= 7.0*" $result
    }

    test {check-acl: --version accepts command from that version} {
        set path [write_acl_file "user alice on >pass ~* +lmpop\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --version 7.0 $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: --version rejects subcommand introduced later (CLIENT|NO-EVICT since 7.0)} {
        set path [write_acl_file "user alice on >pass ~* +client|no-evict\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.2 $path} result
        assert_match "*client|no-evict*requires version >= 7.0*" $result
    }

    test {check-acl: --version accepts subcommand from that version} {
        set path [write_acl_file "user alice on >pass ~* +client|no-evict\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --version 7.0 $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: --version accepts old command (GET since 1.0)} {
        set path [write_acl_file "user alice on >pass ~* +get\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --version 6.0 $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: --version rejects denied subcommand too new} {
        set path [write_acl_file "user alice on >pass ~* +@all -client|no-evict\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.2 $path} result
        assert_match "*Denying subcommands*requires version >= 7.0*" $result
    }

    # ------------------------------------------------------------------
    # --ignore-unknown-commands
    # ------------------------------------------------------------------

    test {check-acl: unknown command rejected by default} {
        set path [write_acl_file "user alice on >pass ~* +mymodule.cmd\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN $path} result
        assert_match "*Unknown command or category*" $result
    }

    test {check-acl: unknown command accepted with --ignore-unknown-commands} {
        set path [write_acl_file "user alice on >pass ~* +mymodule.cmd\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --ignore-unknown-commands $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: mixed known and unknown commands with --ignore-unknown-commands} {
        set path [write_acl_file "user alice on >pass ~* +get +mymodule.cmd +set\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --ignore-unknown-commands $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: known command too new still rejected with --ignore-unknown-commands} {
        set path [write_acl_file "user alice on >pass ~* +lmpop\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --version 6.2 --ignore-unknown-commands $path} result
        assert_match "*Command 'lmpop' requires version >= 7.0*" $result
    }

    test {check-acl: unknown command with --version and --ignore-unknown-commands passes} {
        set path [write_acl_file "user alice on >pass ~* +mymodule.cmd\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --version 7.0 --ignore-unknown-commands $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: unknown category rejected by default} {
        set path [write_acl_file "user alice on >pass ~* +@mycustomcategory\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN $path} result
        assert_match "*Unknown command or category*" $result
    }

    test {check-acl: unknown category rejected even with --ignore-unknown-commands} {
        set path [write_acl_file "user alice on >pass ~* +@mycustomcategory\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --ignore-unknown-commands $path} result
        assert_match "*Unknown command or category*" $result
    }

    # ------------------------------------------------------------------
    # --commands-file
    # ------------------------------------------------------------------

    test {check-acl: --commands-file makes module command known} {
        set cmds [tmpfile "commands"]
        set fd [open $cmds w]
        puts $fd "search.query 0.0.0 read,search"
        close $fd
        set path [write_acl_file "user alice on >pass ~* +search.query\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --commands-file $cmds $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: --commands-file unknown command still rejected} {
        set cmds [tmpfile "commands"]
        set fd [open $cmds w]
        puts $fd "search.query 0.0.0 read,search"
        close $fd
        set path [write_acl_file "user alice on >pass ~* +totally.unknown\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --commands-file $cmds $path} result
        assert_match "*Unknown command*totally.unknown*" $result
    }

    test {check-acl: --commands-file version gating on external command} {
        set cmds [tmpfile "commands"]
        set fd [open $cmds w]
        puts $fd "mymod.newcmd 8.0.0 read,write"
        close $fd
        set path [write_acl_file "user alice on >pass ~* +mymod.newcmd\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --commands-file $cmds --version 7.0 $path} result
        assert_match "*mymod.newcmd*requires version >= 8.0*" $result
    }

    test {check-acl: --commands-file version OK passes} {
        set cmds [tmpfile "commands"]
        set fd [open $cmds w]
        puts $fd "mymod.newcmd 8.0.0 read,write"
        close $fd
        set path [write_acl_file "user alice on >pass ~* +mymod.newcmd\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --commands-file $cmds --version 8.0 $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: multiple --commands-file flags} {
        set cmds1 [tmpfile "commands1"]
        set fd [open $cmds1 w]
        puts $fd "search.query 0.0.0 read,search"
        close $fd
        set cmds2 [tmpfile "commands2"]
        set fd [open $cmds2 w]
        puts $fd "bloom.add 0.0.0 write,bloom"
        close $fd
        set path [write_acl_file "user alice on >pass ~* +search.query +bloom.add\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --commands-file $cmds1 --commands-file $cmds2 $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: --commands-file with comments and empty lines} {
        set cmds [tmpfile "commands"]
        set fd [open $cmds w]
        puts $fd "# This is a comment"
        puts $fd ""
        puts $fd "search.query 0.0.0 read,search"
        puts $fd "# Another comment"
        close $fd
        set path [write_acl_file "user alice on >pass ~* +search.query\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --commands-file $cmds $path]
        assert_match "*ACL file is valid*" $result
    }

    test {check-acl: --commands-file nonexistent file} {
        set path [write_acl_file "user alice on >pass ~* +get\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --commands-file /nonexistent.conf $path} result
        assert_match "*Error opening commands file*" $result
    }

    test {check-acl: --commands-file mixed with built-in commands} {
        set cmds [tmpfile "commands"]
        set fd [open $cmds w]
        puts $fd "search.query 0.0.0 read,search"
        close $fd
        set path [write_acl_file "user alice on >pass ~* +get +set +search.query\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --commands-file $cmds $path]
        assert_match "*ACL file is valid*" $result
    }

    # ------------------------------------------------------------------
    # --simplify
    # ------------------------------------------------------------------

    test {check-acl: --simplify basic output} {
        set path [write_acl_file "user alice on >pass ~* +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --simplify $path]
        assert_match "user alice on*+@all*" $result
    }

    test {check-acl: --simplify removes redundant +cmd after +@all} {
        set path [write_acl_file "user alice on >pass ~* +@all -get +get\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --simplify $path]
        assert_match "*+@all*" $result
        assert_no_match "*+get*" $result
    }

    test {check-acl: --simplify removes +cmd covered by +@category} {
        set path [write_acl_file "user alice on >pass ~* -@all +@read +get\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --simplify $path]
        assert_match "*-@all +@read*" $result
        assert_no_match "*+get*" $result
    }

    test {check-acl: --simplify keeps +cmd as exception to -@category} {
        set path [write_acl_file "user alice on >pass ~* +@all -@string +get\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --simplify $path]
        assert_match "*+@all -@string +get*" $result
    }

    test {check-acl: --simplify keeps meaningful -cmd after +@all} {
        set path [write_acl_file "user alice on >pass ~* +@all -debug\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --simplify $path]
        assert_match "*+@all -debug*" $result
    }

    test {check-acl: --simplify simplifies keys with resetkeys} {
        set path [write_acl_file "user alice on >pass ~a* ~b* resetkeys ~c* +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --simplify $path]
        assert_match "*~c**" $result
        assert_no_match "*~a*" $result
        assert_no_match "*~b*" $result
    }

    test {check-acl: --simplify simplifies state} {
        set path [write_acl_file "user alice on off on >pass ~* +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --simplify $path]
        assert_match "user alice on *" $result
    }

    test {check-acl: --simplify simplifies channels} {
        set path [write_acl_file "user alice on >pass ~* &foo &bar resetchannels &baz +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --simplify $path]
        assert_match "*resetchannels &baz*" $result
        assert_no_match "*&foo*" $result
    }

    test {check-acl: --simplify multiple users} {
        set path [write_acl_file "user alice on >pass ~* +@all -@dangerous\nuser bob on >secret ~cache:* -@all +get +set\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --simplify $path]
        assert_match "*user alice*+@all -@dangerous*" $result
        assert_match "*user bob*-@all +get +set*" $result
    }

    test {check-acl: --simplify with errors does not output simplified} {
        set path [write_acl_file "user alice on >pass ~* +@bogus\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --simplify $path} result
        assert_no_match "*user alice*" $result
        assert_match "*Unknown command or category*" $result
    }

    test {check-acl: --simplify with selectors} {
        set path [write_acl_file "user alice on >pass ~* +get (+@write ~data:*)\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --simplify $path]
        assert_match "*user alice*" $result
        assert_match "*+get*" $result
    }

    test {check-acl: --simplify removes -cmd redundant with -@all} {
        set path [write_acl_file "user alice on >pass ~* -@all -get +set\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --simplify $path]
        assert_match "*-@all*+set*" $result
        assert_no_match "*-get*" $result
    }

    test {check-acl: --simplify normalizes allkeys to ~*} {
        set path [write_acl_file "user alice on >pass allkeys +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --simplify $path]
        assert_match "*~\\**" $result
    }

    test {check-acl: --simplify sorts database IDs} {
        set path [write_acl_file "user alice on >pass ~* db=3,1,2 +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --simplify $path]
        assert_match "*db=1,2,3*" $result
    }

    test {check-acl: --simplify with --quiet only outputs rules} {
        set path [write_acl_file "user alice on >pass ~* +@all\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --simplify --quiet $path]
        assert_match "user alice *" $result
    }

    test {check-acl: --simplify removes -cmd covered by -@category} {
        set path [write_acl_file "user alice on >pass ~* +@all -@dangerous -debug\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --simplify $path]
        assert_match "*+@all -@dangerous*" $result
        assert_no_match "*-debug*" $result
    }

    test {check-acl: --simplify keeps -cmd as exception to +@category} {
        set path [write_acl_file "user alice on >pass ~* -@all +@string -get\n"]
        set result [exec $::VALKEY_CHECK_ACL_BIN --simplify $path]
        assert_match "*-@all +@string -get*" $result
    }

    test {check-acl: --simplify with duplicate user reports error} {
        set path [write_acl_file "user alice on >pass ~* +@all\nuser alice on >pass2 ~* +get\n"]
        catch {exec $::VALKEY_CHECK_ACL_BIN --simplify $path} result
        assert_match "*Duplicate user*" $result
    }
}
