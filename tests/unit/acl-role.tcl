start_server {tags {"acl external:skip"}} {
    test {ACL ROLES - initially empty} {
        r ACL ROLES
    } {}

    test {ACL SETROLE - create a role} {
        r ACL SETROLE myrole ~keys:* +@all -@dangerous
    } {OK}

    test {ACL ROLES - lists the role} {
        r ACL ROLES
    } {myrole}

    test {ACL GETROLE - returns role info} {
        set info [r ACL GETROLE myrole]
        assert_match {*commands*} $info
        assert_match {*keys*} $info
    }

    test {ACL GETROLE - non-existent role returns nil} {
        r ACL GETROLE nonexistent
    } {}

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

    test {ACL SETUSER - add user to role} {
        r ACL SETUSER alice on >pass123 +@role:myrole
    } {OK}

    test {ACL GETUSER - shows role membership} {
        set info [r ACL GETUSER alice]
        set idx [lsearch $info "roles"]
        set roles [lindex $info [expr {$idx + 1}]]
        assert_equal $roles {myrole}
    }

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

    test {ACL SETUSER - remove user from role} {
        r ACL SETUSER alice -@role:myrole
        set info [r ACL GETUSER alice]
        set idx [lsearch $info "roles"]
        set roles [lindex $info [expr {$idx + 1}]]
        assert_equal $roles {}
    }

    test {After removing from role, permissions are revoked} {
        set result [r ACL DRYRUN alice SET keys:test value]
        assert_match {*no permissions*} $result
    }

    test {ACL DELROLE - fails if role has members} {
        r ACL SETUSER bob on >pass456 +@role:otherrole
        catch {r ACL DELROLE otherrole} err
        assert_match {*has members*} $err
    }

    test {ACL DELROLE - succeeds when no members} {
        r ACL SETUSER bob -@role:otherrole
        r ACL DELROLE otherrole
    } {1}

    test {ACL DELROLE - non-existent role returns error} {
        catch {r ACL DELROLE nonexistent} err
        assert_match {*not found*} $err
    }

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

    test {ACL LIST includes roles} {
        set list [r ACL LIST]
        assert_match "role *" [lindex $list 0]
    }

    test {User reset clears role memberships} {
        r ACL SETUSER carol reset
        set info [r ACL GETUSER carol]
        set idx [lsearch $info "roles"]
        set roles [lindex $info [expr {$idx + 1}]]
        assert_equal $roles {}
    }

    test {ACL SETUSER - referencing non-existent role fails} {
        catch {r ACL SETUSER dave on >pass +@role:nosuchrole} err
        assert_match {*role does not exist*} $err
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

    test {Role name validation - no spaces} {
        catch {r ACL SETROLE "bad name" +@all} err
        assert_match {*can't contain spaces*} $err
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
    } {customer readonly}

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
    } {customer readonly}
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
}
