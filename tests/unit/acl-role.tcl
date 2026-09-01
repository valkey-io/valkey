start_server {tags {"acl external:skip"}} {
    test {ACL ROLES - initially empty} {
        r ACL ROLES
    } {}

    # --- ACL SETROLE ---

    test {ACL SETROLE - create a role} {
        r ACL SETROLE myrole ~keys:* +@all -@dangerous
    } {OK}

    test {ACL ROLES - lists the role} {
        r ACL ROLES
    } {myrole}

    test {ACL SETROLE - update existing role} {
        r ACL SETROLE myrole ~keys:* +@all -@dangerous -@scripting
    } {OK}

    test {ACL SETROLE - rejects password operations} {
        catch {r ACL SETROLE myrole >password} err
        assert_match {*Error*} $err
    }

    test {ACL SETROLE - rejects on/off flags} {
        catch {r ACL SETROLE myrole on} err
        assert_match {*Error*} $err

        catch {r ACL SETROLE myrole off} err
        assert_match {*Error*} $err
    }

    test {ACL SETROLE - rejects nested roles} {
        r ACL SETROLE otherrole +@read
        catch {r ACL SETROLE myrole +@role:otherrole} err
        assert_match {*Error*} $err
    }

    test {ACL SETROLE - unmatched parenthesis} {
        catch {r ACL SETROLE badrole (+get} err
        assert_match {*Unmatched parenthesis*} $err
    }

    test {ACL SETROLE - clearselectors removes non-root selectors} {
        r ACL SETROLE multisel +get ~a:* (+set ~b:*)
        r ACL SETROLE multisel clearselectors +get ~a:*
        # After clearselectors, only root selector remains (no extra selectors)
        set info [r ACL GETROLE multisel]
        set idx [lsearch $info "selectors"]
        set sels [lindex $info [expr {$idx + 1}]]
        assert_equal [llength $sels] 0
    }

    test {ACL SETROLE - role name validation - no spaces} {
        catch {r ACL SETROLE "bad name" +@all} err
        assert_match {*Role names can't contain spaces or null characters*} $err
    }

    test {ACL SETROLE - rejects an empty role name} {
        # An empty name survives ACL SETROLE but not a config round trip:
        # sdssplitargs() collapses the whitespace, so the first rule would come
        # back as the role name and the server would refuse to start.
        catch {r ACL SETROLE "" +get ~*} err
        assert_match {*Role names can't be empty*} $err
        assert_equal {} [lsearch -all -inline [r ACL ROLES] {}]
    }

    test {ACL SETROLE - rejects quotes and backslashes in a role name} {
        foreach name {q"x q'x {q\x}} {
            catch {r ACL SETROLE $name +get ~*} err
            assert_match {*Role names can't contain quotes or backslashes*} $err
        }
    }

    test {ACL SETROLE - rejects role name that conflicts with category} {
        catch {r ACL SETROLE read +@all} err
        assert_match {*conflicts with a command or category*} $err

        catch {r ACL SETROLE write +@all} err
        assert_match {*conflicts with a command or category*} $err
    }

    test {ACL SETROLE - rejects role name that conflicts with command} {
        catch {r ACL SETROLE get +@all} err
        assert_match {*conflicts with a command or category*} $err

        catch {r ACL SETROLE set +@all} err
        assert_match {*conflicts with a command or category*} $err

        catch {r ACL SETROLE acl +@all} err
        assert_match {*conflicts with a command or category*} $err
    }

    # --- ACL GETROLE ---

    test {ACL GETROLE - returns role info} {
        set info [r ACL GETROLE myrole]
        assert_match {*commands*} $info
        assert_match {*keys*} $info
    }

    test {ACL GETROLE - non-existent role returns nil} {
        r ACL GETROLE nonexistent
    } {}

    test {ACL GETROLE - shows selectors and members} {
        r ACL SETROLE inforole +get ~info:* (+set ~info:*)
        r ACL SETUSER infouser on >infopass +@role:inforole
        set info [r ACL GETROLE inforole]
        # Check members list
        set idx [lsearch $info "members"]
        set members [lindex $info [expr {$idx + 1}]]
        assert_equal $members {infouser}
        # Check selectors (should have one extra selector beyond root)
        set idx [lsearch $info "selectors"]
        set sels [lindex $info [expr {$idx + 1}]]
        assert_equal [llength $sels] 1
    }

    # --- ACL SETUSER ---

    test {ACL SETUSER - add user to role} {
        r ACL SETUSER alice on >pass123 +@role:myrole
    } {OK}

    test {ACL SETUSER - remove user from role} {
        r ACL SETUSER alice -@role:myrole
        set info [r ACL GETUSER alice]
        set idx [lsearch $info "roles"]
        set roles [lindex $info [expr {$idx + 1}]]
        assert_equal $roles {}
    }

    test {ACL SETUSER - referencing non-existent role fails} {
        catch {r ACL SETUSER dave on >pass +@role:nosuchrole} err
        assert_match {*role does not exist*} $err
    }

    test {ACL SETUSER - remove from non-existent role fails} {
        catch {r ACL SETUSER alice -@role:nosuchrole} err
        assert_match {*role does not exist*} $err
    }

    test {ACL SETUSER - empty role name fails} {
        catch {r ACL SETUSER alice +@role:} err
        assert_match {*Error*} $err

        catch {r ACL SETUSER alice -@role:} err
        assert_match {*Error*} $err
    }

    test {ACL SETUSER - removing a role the user does not hold fails} {
        r ACL SETROLE unheldrole +get ~*
        r ACL SETUSER notamember on >p
        catch {r ACL SETUSER notamember -@role:unheldrole} err
        assert_match {*not a member*} $err
        r ACL DELUSER notamember
        r ACL DELROLE unheldrole
    }

    # --- ACL GETUSER ---

    test {ACL GETUSER - shows role membership} {
        r ACL SETUSER alice +@role:myrole
        set info [r ACL GETUSER alice]
        set idx [lsearch $info "roles"]
        set roles [lindex $info [expr {$idx + 1}]]
        assert_equal $roles {myrole}
    }

    # --- ACL DELROLE ---

    test {ACL DELROLE - fails if role has members} {
        r ACL SETUSER bob on >pass456 +@role:otherrole
        catch {r ACL DELROLE otherrole} err
        assert_match {*has members*} $err
    }

    test {ACL DELROLE - succeeds when no members} {
        r ACL SETUSER bob -@role:otherrole
        r ACL DELROLE otherrole
    } {1}

    test {ACL DELROLE - non-existent role is not counted} {
        r ACL DELROLE nonexistent
    } {0}

    test {ACL DELROLE - delete multiple roles at once} {
        r ACL SETROLE delA +get
        r ACL SETROLE delB +set
        r ACL SETROLE delC +del
        assert_equal [r ACL DELROLE delA delB delC] 3
        # Verify they're all gone
        assert_equal [r ACL GETROLE delA] {}
        assert_equal [r ACL GETROLE delB] {}
        assert_equal [r ACL GETROLE delC] {}
    }

    # --- Permission checks ---

    test {Role permissions are effective for user} {
        r AUTH alice pass123

        r SET keys:hello world
        assert_equal [r GET keys:hello] world

        catch {r SET other:key value} err
        assert_match {*NOPERM*} $err
    } {} {needs:reset}

    test {ACL DRYRUN respects role permissions} {
        r AUTH default ""

        assert_equal [r ACL DRYRUN alice SET keys:test value] {OK}

        set result [r ACL DRYRUN alice SET other:test value]
        assert_match {*no permissions*} $result
    }

    test {After removing from role, permissions are revoked} {
        r ACL SETUSER alice -@role:myrole
        set result [r ACL DRYRUN alice SET keys:test value]
        assert_match {*no permissions*} $result
    }

    test {Role changes are immediately visible to members} {
        r ACL SETROLE liverole +@all ~*
        r ACL SETUSER carol on >carolpass +@role:liverole
        # Carol can do anything now
        assert_equal [r ACL DRYRUN carol SET anykey value] {OK}
        # Update role to restrict keys
        r ACL SETROLE liverole resetkeys +@all ~restricted:*
        # Carol should now only access restricted:* keys
        catch {r ACL DRYRUN carol SET anykey value} err
        assert_match {*no permissions*} $err
        assert_equal [r ACL DRYRUN carol SET restricted:key value] {OK}
    }

    test {Multiple roles - each role is a separate selector with OR logic} {
        r ACL SETROLE roleA +get ~a:*
        r ACL SETROLE roleB +set ~b:*
        r ACL SETUSER multi on >multipass +@role:roleA +@role:roleB

        # roleA allows GET on a:* keys
        assert_equal [r ACL DRYRUN multi GET a:key] {OK}
        # roleB allows SET on b:* keys
        assert_equal [r ACL DRYRUN multi SET b:key value] {OK}

        # GET b:key is denied
        set result [r ACL DRYRUN multi GET b:key]
        assert_match {*no permissions*} $result
        # Keys outside both roles are denied
        set result [r ACL DRYRUN multi GET c:key]
        assert_match {*no permissions*} $result
    }

    test {Role with multiple selectors} {
        # Create a role with two selectors: one for reads on r:*, one for writes on w:*
        r ACL SETROLE multiselector +get ~r:* (+set ~w:*)
        r ACL SETUSER msuser on >mspass +@role:multiselector

        # First selector allows GET on r:*
        assert_equal [r ACL DRYRUN msuser GET r:key] {OK}
        # Second selector allows SET on w:*
        assert_equal [r ACL DRYRUN msuser SET w:key value] {OK}

        # Cross-selector: GET on w:* is denied (no single selector allows it)
        set result [r ACL DRYRUN msuser GET w:key]
        assert_match {*no permissions*} $result
        # SET on r:* is also denied
        set result [r ACL DRYRUN msuser SET r:key value]
        assert_match {*no permissions*} $result
    }

    test {User own permissions add on top of role (OR logic)} {
        r ACL SETROLE onlyset +set ~data:*
        r ACL SETUSER userplus on >pluspass +@role:onlyset +get ~data:*

        # Role allows SET on data:*, user's own selector allows GET on data:*
        assert_equal [r ACL DRYRUN userplus SET data:key value] {OK}
        assert_equal [r ACL DRYRUN userplus GET data:key] {OK}

        # Neither allows DEL
        set result [r ACL DRYRUN userplus DEL data:key]
        assert_match {*no permissions*} $result
    }

    test {User cannot restrict role permissions} {
        r ACL SETROLE permissive +@all ~*
        r ACL SETUSER restricted on >rpass +@role:permissive -@admin

        # Even though user has no admin permissions, the role grants it
        assert_equal [r ACL DRYRUN restricted FLUSHALL] {OK}
    }

    test {Role with channel patterns} {
        r ACL SETROLE channelrole +subscribe &news:* ~*
        r ACL SETUSER chanuser on >chanpass +@role:channelrole
        assert_equal [r ACL DRYRUN chanuser SUBSCRIBE news:sports] {OK}
        set result [r ACL DRYRUN chanuser SUBSCRIBE private:msg]
        assert_match {*no permissions*} $result
    }

    test {SORT BY/GET honours full key access granted by a role} {
        r RPUSH sortlist 1 2 3
        r ACL SETROLE allkeysrole ~* +@all
        r ACL SETROLE onekeyrole ~sortlist +@all
        r ACL SETUSER sortok on >p +@role:allkeysrole
        r ACL SETUSER sortlimited on >p +@role:onekeyrole

        r AUTH sortok p
        assert_equal {1 2 3} [r SORT sortlist BY weight_* GET #]

        r AUTH sortlimited p
        assert_error {*BY option of SORT denied*} {r SORT sortlist BY weight_*}

        r AUTH default ""
    } {OK} {needs:reset}

    # --- ACL LIST ---

    test {ACL LIST includes roles} {
        set list [r ACL LIST]
        assert_match "role *" [lindex $list 0]
    }

    # --- Pubsub client disconnection ---

    test {SETROLE restricting channels kills pubsub clients} {
        r ACL SETROLE pubrole +subscribe &news:* ~*
        r ACL SETUSER pubuser on >pubpass +@role:pubrole
        set rd [valkey_deferring_client]
        $rd AUTH pubuser pubpass
        $rd read
        $rd SUBSCRIBE news:sports
        assert_match {subscribe news:sports 1} [$rd read]

        # Restrict the role's channels
        r ACL SETROLE pubrole resetchannels +subscribe &alerts:* ~*

        # Client should be disconnected
        catch {$rd read} err
        catch {$rd close}
        assert_match {*I/O error*} $err
    }

    test {SETUSER removing role kills pubsub clients using role channels} {
        r ACL SETROLE subrole +subscribe &events:* ~*
        r ACL SETUSER subuser on >subpass +@role:subrole
        set rd [valkey_deferring_client]
        $rd AUTH subuser subpass
        $rd read
        $rd SUBSCRIBE events:live
        assert_match {subscribe events:live 1} [$rd read]

        # Remove user from the role
        r ACL SETUSER subuser -@role:subrole

        # Client should be disconnected
        catch {$rd read} err
        catch {$rd close}
        assert_match {*I/O error*} $err
    }

    # --- User reset ---

    test {User reset clears role memberships} {
        r ACL SETUSER carol reset
        set info [r ACL GETUSER carol]
        set idx [lsearch $info "roles"]
        set roles [lindex $info [expr {$idx + 1}]]
        assert_equal $roles {}
    }

    # --- Case sensitivity of role and user names ---

    test {Role names are case-sensitive} {
        r ACL SETROLE Cache ~c:* +get
        r ACL SETROLE cache ~d:* +set
        assert_equal {Cache cache} [lsort [lsearch -all -inline [r ACL ROLES] {*ache}]]

        r ACL SETUSER caseuser on >p +@role:Cache +@role:cache
        set info [r ACL GETUSER caseuser]
        set idx [lsearch $info "roles"]
        assert_equal {Cache cache} [lsort [lindex $info [expr {$idx + 1}]]]
    }

    test {User names are case-sensitive on the role member list} {
        r ACL SETROLE rr ~* +get
        r ACL SETUSER alice on >p +@role:rr
        r ACL SETUSER ALICE on >p +@role:rr

        set info [r ACL GETROLE rr]
        set idx [lsearch $info "members"]
        assert_equal {ALICE alice} [lsort [lindex $info [expr {$idx + 1}]]]
    }

    # Cleanup
    test {Cleanup test users and roles} {
        # Remove all non-default users (which also drops their role memberships)
        foreach entry [r ACL LIST] {
            if {[string match "user *" $entry]} {
                set uname [lindex $entry 1]
                if {$uname ne "default"} {
                    catch {r ACL DELUSER $uname}
                }
            }
        }
        # Now delete all roles (no members left)
        foreach role [r ACL ROLES] {
            catch {r ACL DELROLE $role}
        }
    }
}

# Test loading roles from ACL file
set server_path [tmpdir "server.role.acl"]
exec cp -f tests/assets/role.acl $server_path
start_server [list overrides [list "dir" $server_path "aclfile" "role.acl"] tags [list "external:skip"]] {

    test {Roles loaded from ACL file} {
        lsort [r ACL ROLES]
    } {customer viewer}

    test {Users loaded with role assignments from ACL file} {
        set info [r ACL GETUSER alice]
        set idx [lsearch $info "roles"]
        set roles [lindex $info [expr {$idx + 1}]]
        assert_equal $roles {customer}
    }

    test {Role permissions work after loading from ACL file} {
        # alice has customer role: all commands except admin/dangerous/scripting
        assert_equal [r ACL DRYRUN alice SET anykey value] {OK}

        set result [r ACL DRYRUN alice FLUSHALL]
        assert_match {*no permissions*} $result
    }

    test {User-level permissions add on top of role from ACL file} {
        assert_equal [r ACL DRYRUN carol EVAL "return 1" 0] {OK}
        set result [r ACL DRYRUN alice EVAL "return 1" 0]
        assert_match {*no permissions*} $result
    }

    test {ACL SAVE and reload preserves roles} {
        r ACL SAVE
        r ACL LOAD
        lsort [r ACL ROLES]
    } {customer viewer}

    test {Default user keeps its role membership across ACL LOAD} {
        for {set i 0} {$i < 3} {incr i} {
            r ACL LOAD

            set info [r ACL GETUSER default]
            set idx [lsearch $info "roles"]
            assert_equal {viewer} [lindex $info [expr {$idx + 1}]]

            set info [r ACL GETROLE viewer]
            set idx [lsearch $info "members"]
            assert_equal {bob default} [lsort [lindex $info [expr {$idx + 1}]]]
        }
    }

    test {Role held by the default user cannot be deleted} {
        r ACL SETUSER bob -@role:viewer
        catch {r ACL DELROLE viewer} err
        assert_match {*has members*} $err

        # Reading the user back must not dereference a stale role entry.
        assert_match {*+@role:viewer*} [r ACL LIST]
        assert_equal {PONG} [r PING]
    }
}

# Test ACL file error paths for roles
set server_path [tmpdir "server.role.errors.acl"]
exec cp -f tests/assets/role.acl $server_path
start_server [list overrides [list "dir" $server_path "aclfile" "role.acl"] tags [list "external:skip"]] {

    test {ACL LOAD - role with invalid rules fails} {
        set fd [open "$server_path/role.acl" w]
        puts $fd "role badrole >password"
        close $fd
        catch {r ACL LOAD} err
        assert_match {*Error*} $err
    }

    test {ACL LOAD - role line without name fails} {
        set fd [open "$server_path/role.acl" w]
        puts $fd "role"
        close $fd
        catch {r ACL LOAD} err
        assert_match {*requires a role name*} $err
    }

    test {ACL LOAD - duplicate role fails} {
        set fd [open "$server_path/role.acl" w]
        puts $fd "role dup ~* +@all"
        puts $fd "role dup ~* +@read"
        close $fd
        catch {r ACL LOAD} err
        assert_match {*Duplicate role*} $err
    }

    test {Restore valid ACL file} {
        exec cp -f tests/assets/role.acl $server_path
        r ACL LOAD
    }
}

# Test loading roles from valkey.conf inline directives
set conf_lines [list "role" "inlinerole ~* +@read" "user" "inlineuser on >ipass +@role:inlinerole"]
start_server [list config_lines $conf_lines tags [list "external:skip"]] {

    test {Roles loaded from valkey.conf inline directives} {
        r ACL ROLES
    } {inlinerole}

    test {User with role from valkey.conf works} {
        assert_equal [r ACL DRYRUN inlineuser GET anykey] {OK}

        set result [r ACL DRYRUN inlineuser SET anykey value]
        assert_match {*no permissions*} $result
    }

    test {CONFIG REWRITE persists runtime role changes} {
        r ACL SETROLE runtimerole ~rt:* +get
        r CONFIG REWRITE
        assert_match {*role runtimerole*} [exec cat [srv 0 config_file]]
    }

    test {CONFIG REWRITE drops roles deleted at runtime} {
        r ACL DELROLE runtimerole
        r CONFIG REWRITE
        assert_equal 0 [string match {*role runtimerole*} [exec cat [srv 0 config_file]]]
    }
}

# Test duplicate role in config on startup
test {Duplicate role in config on startup fails} {
    catch {exec $::VALKEY_SERVER_BIN --role dup --role dup} err
    assert_match {*Duplicate role*} $err
} {} {external:skip}

# Test invalid role name in config on startup
test {Invalid role name in config on startup fails} {
    catch {exec $::VALKEY_SERVER_BIN --role "" +get} err
    assert_match {*Role names can't be empty*} $err

    set tmpdir [tmpdir "role-invalid-name.acl"]
    set fd [open "$tmpdir/role.acl" w]
    puts $fd {role q"x +get}
    close $fd
    catch {exec $::VALKEY_SERVER_BIN --aclfile "$tmpdir/role.acl"} err
    assert_match {*invalid role name*quotes or backslashes*} $err
} {} {external:skip}

# Test invalid role rule in config on startup
test {Invalid role rule in config on startup fails} {
    set tmpdir [tmpdir "badrole.conf"]
    set fd [open "$tmpdir/valkey.conf" w]
    puts $fd "role badrole >password"
    close $fd
    catch {exec $::VALKEY_SERVER_BIN "$tmpdir/valkey.conf"} err
    assert_match {*Error in role declaration*} $err
} {} {external:skip}

# Test role name conflicting with category/command in config on startup
test {Role name conflicting with category in config on startup fails} {
    set tmpdir [tmpdir "role-category-conflict.conf"]
    set fd [open "$tmpdir/valkey.conf" w]
    puts $fd "role read +@all"
    close $fd
    catch {exec $::VALKEY_SERVER_BIN "$tmpdir/valkey.conf"} err
    assert_match {*conflicts with a command or category*} $err
} {} {external:skip}

test {Role name conflicting with command in config on startup fails} {
    set tmpdir [tmpdir "role-command-conflict.conf"]
    set fd [open "$tmpdir/valkey.conf" w]
    puts $fd "role get +@all"
    close $fd
    catch {exec $::VALKEY_SERVER_BIN "$tmpdir/valkey.conf"} err
    assert_match {*conflicts with a command or category*} $err
} {} {external:skip}
