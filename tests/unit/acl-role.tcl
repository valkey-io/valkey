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
        catch {r ACL SETROLE myrole role=otherrole} err
        assert_match {*Error*} $err

        # resetroles is a user rule too, a role has no roles to reset.
        catch {r ACL SETROLE myrole resetroles} err
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

    test {ACL SETROLE - role names accept printable ASCII} {
        foreach name {read-only read_only app.reader v2 role:admin a=b} {
            r ACL SETROLE $name +get ~*
            assert_not_equal -1 [lsearch -exact [r ACL ROLES] $name]
        }
        # The names round trip through a user's role= list unchanged.
        r ACL SETUSER asciiuser on >p role=read-only,app.reader,a=b
        set info [r ACL GETUSER asciiuser]
        set idx [lsearch $info "roles"]
        assert_equal {a=b app.reader read-only} [lsort [lindex $info [expr {$idx + 1}]]]
        r ACL DELUSER asciiuser
        r ACL DELROLE read-only read_only app.reader v2 role:admin a=b
    }

    test {ACL SETROLE - role names reject what the parsers cannot read back} {
        # Space and the other control characters end a token, a comma separates
        # the names in a `role=` list, and quotes and backslashes are special to
        # sdssplitargs(), which reads the ACL file and valkey.conf back.
        foreach {name reason} {
            {bad name}     {*printable ASCII*}
            "tab\there"    {*printable ASCII*}
            "test\xc3\xa9" {*printable ASCII*}
            a,b            {*can't contain commas*}
            q"x            {*quotes or backslashes*}
            q'x            {*quotes or backslashes*}
            {q\x}          {*quotes or backslashes*}
        } {
            catch {r ACL SETROLE $name +@all} err
            assert_match $reason $err
            assert_equal -1 [lsearch -exact [r ACL ROLES] $name]
        }
    }

    test {ACL SETROLE - rejects an empty role name} {
        # An empty name survives ACL SETROLE but not a config round trip:
        # sdssplitargs() collapses the whitespace, so the first rule would come
        # back as the role name and the server would refuse to start.
        catch {r ACL SETROLE "" +get ~*} err
        assert_match {*Role names can't be empty*} $err
        assert_equal {} [lsearch -all -inline [r ACL ROLES] {}]
    }

    test {ACL SETROLE - a role name may collide with a command or category} {
        # `role=` keeps role names in their own namespace, so there is nothing
        # to disambiguate against commands and categories.
        r ACL SETROLE get ~g:* +get
        r ACL SETROLE read ~r:* +get
        r ACL SETUSER collide on >p role=get,read
        assert_equal [r ACL DRYRUN collide GET g:key] {OK}
        assert_equal [r ACL DRYRUN collide GET r:key] {OK}
        r ACL DELUSER collide
        r ACL DELROLE get read
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

    test {ACL GETROLE - shows selectors and users} {
        r ACL SETROLE inforole +get ~info:* (+set ~info:*)
        r ACL SETUSER infouser on >infopass role=inforole
        set info [r ACL GETROLE inforole]
        # Check the list of users holding the role
        set idx [lsearch $info "users"]
        set users [lindex $info [expr {$idx + 1}]]
        assert_equal $users {infouser}
        # Check selectors (should have one extra selector beyond root)
        set idx [lsearch $info "selectors"]
        set sels [lindex $info [expr {$idx + 1}]]
        assert_equal [llength $sels] 1
    }

    # --- ACL SETUSER ---

    test {ACL SETUSER - assign a role to a user} {
        r ACL SETUSER alice on >pass123 role=myrole
    } {OK}

    test {ACL SETUSER - resetroles removes every role} {
        r ACL SETUSER alice resetroles
        set info [r ACL GETUSER alice]
        set idx [lsearch $info "roles"]
        set roles [lindex $info [expr {$idx + 1}]]
        assert_equal $roles {}
    }

    test {ACL SETUSER - role= replaces the whole set rather than adding to it} {
        r ACL SETROLE repA +get ~a:*
        r ACL SETROLE repB +get ~b:*
        r ACL SETUSER repuser on >p role=repA,repB
        set info [r ACL GETUSER repuser]
        set idx [lsearch $info "roles"]
        assert_equal {repA repB} [lsort [lindex $info [expr {$idx + 1}]]]

        r ACL SETUSER repuser role=repB
        set info [r ACL GETUSER repuser]
        set idx [lsearch $info "roles"]
        assert_equal {repB} [lindex $info [expr {$idx + 1}]]

        # repA no longer lists the user, so it can be deleted.
        assert_equal 1 [r ACL DELROLE repA]
        r ACL SETUSER repuser resetroles
        r ACL DELUSER repuser
        r ACL DELROLE repB
    }

    test {ACL SETUSER - the same role named twice is kept once} {
        r ACL SETROLE dupe +get ~*
        r ACL SETUSER dupeuser on >p role=dupe,dupe
        set info [r ACL GETUSER dupeuser]
        set idx [lsearch $info "roles"]
        assert_equal {dupe} [lindex $info [expr {$idx + 1}]]
        r ACL DELUSER dupeuser
        r ACL DELROLE dupe
    }

    test {ACL SETUSER - referencing non-existent role fails} {
        catch {r ACL SETUSER dave on >pass role=nosuchrole} err
        assert_match {*role does not exist*} $err
    }

    test {ACL SETUSER - a failing role= leaves the user's roles untouched} {
        r ACL SETROLE keptrole +get ~kept:*
        r ACL SETUSER keeper on >p role=keptrole
        catch {r ACL SETUSER keeper role=keptrole,nosuchrole} err
        assert_match {*role does not exist*} $err
        set info [r ACL GETUSER keeper]
        set idx [lsearch $info "roles"]
        assert_equal {keptrole} [lindex $info [expr {$idx + 1}]]
        r ACL DELUSER keeper
        r ACL DELROLE keptrole
    }

    test {ACL SETUSER - empty and malformed role= lists are rejected} {
        r ACL SETROLE listrole +get ~*
        # role= has to name at least one role. resetroles is the way to leave a
        # user with none.
        foreach spec {role= role=, role=,listrole role=listrole, role=listrole,,listrole} {
            catch {r ACL SETUSER alice $spec} err
            assert_match {*Syntax error*} $err
        }
        r ACL DELROLE listrole
    }

    # --- ACL GETUSER ---

    test {ACL GETUSER - shows role membership} {
        r ACL SETUSER alice role=myrole
        set info [r ACL GETUSER alice]
        set idx [lsearch $info "roles"]
        set roles [lindex $info [expr {$idx + 1}]]
        assert_equal $roles {myrole}
    }

    # --- ACL DELROLE ---

    test {ACL DELROLE - fails if the role is assigned to a user} {
        r ACL SETUSER bob on >pass456 role=otherrole
        catch {r ACL DELROLE otherrole} err
        assert_match {*is assigned to one or more users*} $err
    }

    test {ACL DELROLE - succeeds when no user holds the role} {
        r ACL SETUSER bob resetroles
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
        r ACL SETUSER alice resetroles
        set result [r ACL DRYRUN alice SET keys:test value]
        assert_match {*no permissions*} $result
    }

    test {Role changes are immediately visible to the users holding it} {
        r ACL SETROLE liverole +@all ~*
        r ACL SETUSER carol on >carolpass role=liverole
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
        r ACL SETUSER multi on >multipass role=roleA,roleB

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
        r ACL SETUSER msuser on >mspass role=multiselector

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
        r ACL SETUSER userplus on >pluspass role=onlyset +get ~data:*

        # Role allows SET on data:*, user's own selector allows GET on data:*
        assert_equal [r ACL DRYRUN userplus SET data:key value] {OK}
        assert_equal [r ACL DRYRUN userplus GET data:key] {OK}

        # Neither allows DEL
        set result [r ACL DRYRUN userplus DEL data:key]
        assert_match {*no permissions*} $result
    }

    test {User cannot restrict role permissions} {
        r ACL SETROLE permissive +@all ~*
        r ACL SETUSER restricted on >rpass role=permissive -@admin

        # Even though user has no admin permissions, the role grants it
        assert_equal [r ACL DRYRUN restricted FLUSHALL] {OK}
    }

    test {Role with channel patterns} {
        r ACL SETROLE channelrole +subscribe &news:* ~*
        r ACL SETUSER chanuser on >chanpass role=channelrole
        assert_equal [r ACL DRYRUN chanuser SUBSCRIBE news:sports] {OK}
        set result [r ACL DRYRUN chanuser SUBSCRIBE private:msg]
        assert_match {*no permissions*} $result
    }

    test {SORT BY/GET honours full key access granted by a role} {
        r RPUSH sortlist 1 2 3
        r ACL SETROLE allkeysrole ~* +@all
        r ACL SETROLE onekeyrole ~sortlist +@all
        r ACL SETUSER sortok on >p role=allkeysrole
        r ACL SETUSER sortlimited on >p role=onekeyrole

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
        r ACL SETUSER pubuser on >pubpass role=pubrole
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

    test {SETROLE restricting channels kills shard pubsub clients} {
        r ACL SETROLE shardrole +ssubscribe &shard:* ~*
        r ACL SETUSER sharduser on >shardpass role=shardrole
        set rd [valkey_deferring_client]
        $rd AUTH sharduser shardpass
        $rd read
        $rd SSUBSCRIBE shard:one
        assert_match {ssubscribe shard:one 1} [$rd read]

        r ACL SETROLE shardrole resetchannels +ssubscribe &other:* ~*

        catch {$rd read} err
        catch {$rd close}
        assert_match {*I/O error*} $err
    }

    test {SETUSER removing role kills pubsub clients using role channels} {
        r ACL SETROLE subrole +subscribe &events:* ~*
        r ACL SETUSER subuser on >subpass role=subrole
        set rd [valkey_deferring_client]
        $rd AUTH subuser subpass
        $rd read
        $rd SUBSCRIBE events:live
        assert_match {subscribe events:live 1} [$rd read]

        # Remove user from the role
        r ACL SETUSER subuser resetroles

        # Client should be disconnected
        catch {$rd read} err
        catch {$rd close}
        assert_match {*I/O error*} $err
    }

    # --- User reset ---

    test {ACL DELUSER removes the user from the role user list} {
        r ACL SETROLE delrole ~* +get
        r ACL SETUSER deluser1 on >p role=delrole
        r ACL SETUSER deluser2 on >p role=delrole

        set info [r ACL GETROLE delrole]
        set idx [lsearch $info "users"]
        assert_equal {deluser1 deluser2} [lsort [lindex $info [expr {$idx + 1}]]]

        r ACL DELUSER deluser1
        set info [r ACL GETROLE delrole]
        set idx [lsearch $info "users"]
        assert_equal {deluser2} [lindex $info [expr {$idx + 1}]]

        # With the last user gone the role becomes deletable.
        r ACL DELUSER deluser2
        assert_equal 1 [r ACL DELROLE delrole]
    }

    test {User reset clears role memberships} {
        r ACL SETUSER carol reset
        set info [r ACL GETUSER carol]
        set idx [lsearch $info "roles"]
        set roles [lindex $info [expr {$idx + 1}]]
        assert_equal $roles {}
    }

    # --- Roles are not users ---

    test {A role cannot be authenticated against or read as a user} {
        r ACL SETROLE notauser ~* +@all
        catch {r AUTH notauser anything} err
        assert_match {*WRONGPASS*} $err

        # Roles live in their own table, so the user commands must not see them
        # and the role commands must not see users.
        assert_equal {} [r ACL GETUSER notauser]
        assert_equal {} [r ACL GETROLE default]
        assert_equal -1 [lsearch -exact [r ACL USERS] notauser]
        assert_equal -1 [lsearch -exact [r ACL ROLES] default]
        r ACL DELROLE notauser
    }

    test {Role subcommands require admin permissions} {
        r ACL SETROLE probed ~* +get
        r ACL SETUSER plain on >p ~* +@all -@admin -@dangerous

        assert_match {*no permissions*} [r ACL DRYRUN plain ACL SETROLE x +get]
        assert_match {*no permissions*} [r ACL DRYRUN plain ACL DELROLE probed]
        assert_match {*no permissions*} [r ACL DRYRUN plain ACL GETROLE probed]
        assert_match {*no permissions*} [r ACL DRYRUN plain ACL ROLES]
        r ACL DELROLE probed
    }

    test {ACL LOG records a denial for a user whose access comes from a role} {
        r ACL LOG RESET
        r ACL SETROLE logrole ~allowed:* +get
        r ACL SETUSER loguser on >logpass role=logrole

        set rd [valkey_client]
        $rd AUTH loguser logpass
        catch {$rd GET denied:key} err
        assert_match {*NOPERM*} $err
        $rd close

        set entry [lindex [r ACL LOG] 0]
        assert_equal [dict get $entry username] {loguser}
        assert_equal [dict get $entry context] {toplevel}
        assert_equal [dict get $entry reason] {key}
        assert_equal [dict get $entry object] {denied:key}
    }

    # --- Case sensitivity of role and user names ---

    test {Role names are case-sensitive} {
        r ACL SETROLE Cache ~c:* +get
        r ACL SETROLE cache ~d:* +set
        assert_equal {Cache cache} [lsort [lsearch -all -inline [r ACL ROLES] {*ache}]]

        r ACL SETUSER caseuser on >p role=Cache,cache
        set info [r ACL GETUSER caseuser]
        set idx [lsearch $info "roles"]
        assert_equal {Cache cache} [lsort [lindex $info [expr {$idx + 1}]]]
    }

    test {User names are case-sensitive on the role user list} {
        r ACL SETROLE rr ~* +get
        r ACL SETUSER alice on >p role=rr
        r ACL SETUSER ALICE on >p role=rr

        set info [r ACL GETROLE rr]
        set idx [lsearch $info "users"]
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
        # Now delete all roles (no users hold them any more)
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

    test {ACL SAVE and reload preserves a punctuated role name} {
        r ACL SETROLE app.read-only ~ro:* +get
        r ACL SETUSER punctuser on >p role=app.read-only,customer
        r ACL SAVE
        r ACL LOAD

        assert_equal {app.read-only customer viewer} [lsort [r ACL ROLES]]
        set info [r ACL GETUSER punctuser]
        set idx [lsearch $info "roles"]
        assert_equal {app.read-only customer} [lsort [lindex $info [expr {$idx + 1}]]]
        assert_equal [r ACL DRYRUN punctuser GET ro:key] {OK}

        r ACL DELUSER punctuser
        r ACL DELROLE app.read-only
        r ACL SAVE
    }

    test {Default user keeps its role membership across ACL LOAD} {
        for {set i 0} {$i < 3} {incr i} {
            r ACL LOAD

            set info [r ACL GETUSER default]
            set idx [lsearch $info "roles"]
            assert_equal {viewer} [lindex $info [expr {$idx + 1}]]

            set info [r ACL GETROLE viewer]
            set idx [lsearch $info "users"]
            assert_equal {bob default} [lsort [lindex $info [expr {$idx + 1}]]]
        }
    }

    test {Role held by the default user cannot be deleted} {
        r ACL SETUSER bob resetroles
        catch {r ACL DELROLE viewer} err
        assert_match {*is assigned to one or more users*} $err

        # Reading the user back must not dereference a stale role entry.
        assert_match {*role=viewer*} [r ACL LIST]
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
set conf_lines [list "role" "inlinerole ~* +@read" "user" "inlineuser on >ipass role=inlinerole"]
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

    test {A user's role= survives CONFIG REWRITE and a restart} {
        # Punctuated names are the interesting case: they have to come back
        # from sdssplitargs() as one token and split on the comma the same way.
        r ACL SETROLE app.read-only ~rw:* +get
        r ACL SETROLE second ~sc:* +set
        r ACL SETUSER rewriteuser on >p role=app.read-only,second
        r CONFIG REWRITE
        restart_server 0 true false

        assert_equal {app.read-only inlinerole second} [lsort [r ACL ROLES]]
        set info [r ACL GETUSER rewriteuser]
        set idx [lsearch $info "roles"]
        assert_equal {app.read-only second} [lsort [lindex $info [expr {$idx + 1}]]]
        assert_equal [r ACL DRYRUN rewriteuser GET rw:key] {OK}
        assert_equal [r ACL DRYRUN rewriteuser SET sc:key v] {OK}

        # The roles are still held, so they cannot be deleted yet.
        assert_error {*is assigned to one or more users*} {r ACL DELROLE app.read-only}
        r ACL DELUSER rewriteuser
        r ACL DELROLE app.read-only second
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

    catch {exec $::VALKEY_SERVER_BIN --aclfile tests/assets/role-invalid-name.acl} err
    assert_match {*invalid role name*commas*} $err
} {} {external:skip}

# Test invalid role rule in config on startup
test {Invalid role rule in config on startup fails} {
    catch {exec $::VALKEY_SERVER_BIN tests/assets/role-invalid-rule.conf} err
    assert_match {*Error in role declaration*} $err
} {} {external:skip}
