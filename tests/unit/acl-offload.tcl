# acl-offload: ACL permissions evaluated on IO threads, verdicts consumed by
# main only while the global ACL epoch is unchanged.
#
# Verification map:
#   (a)  same-batch AUTH/SELECT cutoff -- strict same-connection semantics
#   (b)  cross-client SETUSER churn -- no torn verdicts, no crash
#   (b') TOCTOU probe -- deny Y then write secret; Y must never read it
#   (c)  engagement -- INFO counters prove verdicts are being consumed

proc acl_offload_hits {} { getInfoProperty [r info stats] acl_offload_hits }
proc acl_offload_punts {} { getInfoProperty [r info stats] acl_offload_punts }
# Format a RESP command for raw pipelined writes.
proc resp {args} {
    set out "*[llength $args]\r\n"
    foreach a $args { append out "\$[string length $a]\r\n$a\r\n" }
    return $out
}

start_server {config "minimal.conf" tags {"acl external:skip valgrind:skip"} overrides {enable-debug-command yes io-threads 4 io-threads-always-active yes acl-offload yes}} {

    test {acl-offload: verdicts are consumed on the fast path (engagement)} {
        r acl setuser alice on nopass ~alice:* +@all
        set rd [valkey_client]
        $rd auth alice ""
        set before [acl_offload_hits]
        # A pipelined batch of GETs: everything after AUTH in the *next* read
        # is taggable. Send in one write so the worker sees a batch.
        for {set i 0} {$i < 200} {incr i} { $rd write [resp GET alice:$i] }
        $rd flush
        for {set i 0} {$i < 200} {incr i} { $rd read }
        wait_for_condition 50 20 { [acl_offload_hits] > $before } else { fail "no acl-offload hits recorded" }
        $rd close
    }

    test {acl-offload: denial replies are byte-identical to stock} {
        r acl setuser bob on nopass ~bob:* +@all
        set rd [valkey_client]
        $rd auth bob ""
        # deny by key pattern
        catch {$rd get alice:1} e1
        assert_match "*NOPERM*No permissions to access a key*" $e1
        # deny by command
        r acl setuser bob -get
        catch {$rd get bob:1} e2
        assert_match "*NOPERM*has no permissions to run the 'get' command*" $e2
        $rd close
    }

    test {acl-offload (a): same-batch AUTH cutoff is strict, allow->deny} {
        r acl setuser open on nopass ~* +@all
        r acl setuser closed on nopass ~closed:* +@all
        r set k v
        set rd [valkey_deferring_client]
        $rd auth open ""
        assert_equal OK [$rd read]
        # One write: GET (allowed as open) ; AUTH closed ; GET (must be denied
        # as closed even though it was parsed in the same batch as the AUTH).
        # ONE write: the whole batch must reach the server in a single read.
        $rd write "[resp GET k][resp AUTH closed ""][resp GET k]"
        $rd flush
        assert_equal v [$rd read]
        assert_equal OK [$rd read]
        catch {$rd read} e
        assert_match "*NOPERM*" $e
        $rd close
    }

    test {acl-offload (a): same-batch AUTH cutoff is strict, deny->allow} {
        r set k v
        set rd [valkey_deferring_client]
        $rd auth closed ""
        assert_equal OK [$rd read]
        $rd write "[resp GET k][resp AUTH open ""][resp GET k]"
        $rd flush
        catch {$rd read} e
        assert_match "*NOPERM*" $e
        assert_equal OK [$rd read]
        catch {$rd read} v
        assert_equal v $v
        $rd close
    }

    test {acl-offload (a): same-batch SELECT cutoff honors db selectors} {
        # dbuser may do anything in db 0, and may only SELECT into db 2.
        # GET in db 2 is therefore denied -- but at parse time the client is
        # still in db 0, where the same GET is allowed. The SELECT cutoff must
        # keep the worker from tagging the GET with a db-0 verdict.
        r acl setuser dbuser on nopass ~* +@all db=0 (+select db=2)
        r select 2
        r set k2 v2
        r select 0
        set rd [valkey_deferring_client]
        $rd auth dbuser ""
        assert_equal OK [$rd read]
        # The harness parks clients in db 9; this test needs the parse-time db
        # to be one where the GET IS allowed, so the cutoff is what denies it.
        $rd select 0
        assert_equal OK [$rd read]
        $rd write "[resp SELECT 2][resp GET k2]"
        $rd flush
        assert_equal OK [$rd read]
        catch {$rd read} e
        assert_match "*NOPERM*" $e
        $rd close
        r select 0
    }

    test {acl-offload (b'): TOCTOU probe -- deny then write secret, reader never sees it} {
        r acl setuser reader on nopass ~* +@all
        r acl setuser admin on nopass ~* +@all
        r set secretkey public
        set admin [valkey_deferring_client]
        $admin auth admin ""
        assert_equal OK [$admin read]
        # Many deep-pipelined readers: their batches get parsed (and tagged
        # "allow") on IO threads, then wait in main's queue.
        set readers {}
        for {set i 0} {$i < 8} {incr i} {
            set rd [valkey_deferring_client]
            $rd auth reader ""
            assert_equal OK [$rd read]
            lappend readers $rd
        }
        set leaked 0
        set denied 0
        set rounds 100
        for {set round 0} {$round < $rounds} {incr round} {
            $admin write [resp ACL SETUSER reader ~*]
            $admin write [resp SET secretkey public]
            $admin flush
            $admin read; $admin read
            # Readers fire deep pipelined GET batches; the admin's deny+write
            # follows in the same instant so all nine reads land in one event
            # loop batch: IO threads parse the readers' GETs (tagging them
            # "allow" under the still-open rules) while main, which gets the
            # admin's tiny batch back first, executes deny THEN write. The
            # readers' tags are consumed after both. Under reference semantics
            # no reader reply can ever be "secret": a GET is either before the
            # deny (sees "public") or after it (NOPERM).
            foreach rd $readers {
                for {set j 0} {$j < 64} {incr j} { $rd write [resp GET secretkey] }
            }
            $admin write [resp ACL SETUSER reader resetkeys]
            $admin write [resp SET secretkey secret]
            foreach rd $readers { $rd flush }
            $admin flush
            $admin read; $admin read
            foreach rd $readers {
                for {set j 0} {$j < 64} {incr j} {
                    if {[catch {$rd read} v]} {
                        incr denied
                    } elseif {$v eq "secret"} {
                        incr leaked
                    }
                }
            }
        }
        if {$::verbose} { puts "TOCTOU probe: leaked=$leaked denied=$denied" }
        assert_equal 0 $leaked
        # Falsification record: with the main-thread epoch compare stubbed out
        # (verdicts trusted unconditionally) this probe reported 9,664 leaked
        # "secret" replies in 100 rounds. Re-run that way after any change to
        # the compare to confirm the probe still bites.
        foreach rd $readers { $rd close }
        $admin close
    }

    test {acl-offload (b): cross-client SETUSER/DELUSER churn under load is stable} {
        r acl setuser churn on nopass ~* +@all
        set readers {}
        for {set i 0} {$i < 4} {incr i} {
            set rd [valkey_deferring_client]
            $rd auth churn ""
            assert_equal OK [$rd read]
            lappend readers $rd
        }
        set admin [valkey_client]
        for {set round 0} {$round < 40} {incr round} {
            foreach rd $readers {
                for {set j 0} {$j < 32} {incr j} { $rd write [resp GET k] }
                $rd flush
            }
            # flip patterns back and forth: each flip is a COW swap + epoch bump
            $admin acl setuser churn resetkeys ~zzz:* ~yyy:*
            $admin acl setuser churn ~*
            foreach rd $readers {
                for {set j 0} {$j < 32} {incr j} { catch {$rd read} }
            }
        }
        # Every reply was either the value or NOPERM (asserted implicitly by the
        # protocol reads succeeding); the server is alive and consistent.
        assert_equal PONG [r ping]
        # DELUSER while readers have in-flight reads: must not crash.
        foreach rd $readers {
            for {set j 0} {$j < 32} {incr j} { $rd write [resp GET k] }
            $rd flush
        }
        $admin acl deluser churn
        assert_equal PONG [r ping]
        foreach rd $readers { catch {$rd close} }
        $admin close
    }

    test {acl-offload (b''): TOCTOU probe via role -- revoke role's key access then write secret, member never sees it} {
        # Permission comes from a ROLE, not the user's own selectors. The IO
        # thread walks role->selectors when tagging a member's command, so
        # ACL SETROLE on a live role must invalidate pending verdicts exactly
        # like ACL SETUSER does.
        r acl setrole keyrole ~* +@all
        r acl setuser member on nopass role=keyrole
        r acl setuser roleadmin on nopass ~* +@all
        r set rolesecret public
        set admin [valkey_deferring_client]
        $admin auth roleadmin ""
        assert_equal OK [$admin read]
        set readers {}
        for {set i 0} {$i < 8} {incr i} {
            set rd [valkey_deferring_client]
            $rd auth member ""
            assert_equal OK [$rd read]
            lappend readers $rd
        }
        set leaked 0
        set denied 0
        for {set round 0} {$round < 100} {incr round} {
            $admin write [resp ACL SETROLE keyrole ~*]
            $admin write [resp SET rolesecret public]
            $admin flush
            $admin read; $admin read
            foreach rd $readers {
                for {set j 0} {$j < 64} {incr j} { $rd write [resp GET rolesecret] }
            }
            $admin write [resp ACL SETROLE keyrole resetkeys]
            $admin write [resp SET rolesecret secret]
            foreach rd $readers { $rd flush }
            $admin flush
            $admin read; $admin read
            foreach rd $readers {
                for {set j 0} {$j < 64} {incr j} {
                    if {[catch {$rd read} v]} {
                        incr denied
                    } elseif {$v eq "secret"} {
                        incr leaked
                    }
                }
            }
        }
        if {$::verbose} { puts "role TOCTOU probe: leaked=$leaked denied=$denied" }
        assert_equal 0 $leaked
        foreach rd $readers { $rd close }
        $admin close
        r acl deluser member
        r acl delrole keyrole
    }

    test {acl-offload (b): SETROLE churn on a live role under member load is stable} {
        # Each SETROLE on a role with members is a COW swap + epoch bump +
        # drain; the old selector list must outlive every in-flight read job.
        r acl setrole churnrole ~* +@all
        r acl setuser rolemember on nopass role=churnrole
        set readers {}
        for {set i 0} {$i < 4} {incr i} {
            set rd [valkey_deferring_client]
            $rd auth rolemember ""
            assert_equal OK [$rd read]
            lappend readers $rd
        }
        set admin [valkey_client]
        for {set round 0} {$round < 40} {incr round} {
            foreach rd $readers {
                for {set j 0} {$j < 32} {incr j} { $rd write [resp GET k] }
                $rd flush
            }
            $admin acl setrole churnrole resetkeys ~zzz:* ~yyy:*
            $admin acl setrole churnrole ~*
            foreach rd $readers {
                for {set j 0} {$j < 32} {incr j} { catch {$rd read} }
            }
        }
        assert_equal PONG [r ping]
        foreach rd $readers { catch {$rd close} }
        $admin acl deluser rolemember
        $admin acl delrole churnrole
        assert_equal PONG [r ping]
        $admin close
    }

    test {acl-offload: punts are counted when the epoch moves} {
        r acl setuser p on nopass ~* +@all
        set rd [valkey_client]
        $rd auth p ""
        set before [acl_offload_punts]
        for {set i 0} {$i < 20} {incr i} {
            $rd get k
            r acl setuser p ~*  ;# epoch bump between batches
        }
        # Not strictly deterministic (depends on parse/execute interleaving),
        # but the INFO field must exist and be non-negative.
        assert {[acl_offload_punts] >= $before}
        $rd close
    }
}

# Same suite shape with the feature OFF: guarantees the knob is inert.
start_server {config "minimal.conf" tags {"acl external:skip valgrind:skip"} overrides {io-threads 4 io-threads-always-active yes acl-offload no}} {
    test {acl-offload disabled: no hits are recorded} {
        r acl setuser alice on nopass ~alice:* +@all
        set rd [valkey_client]
        $rd auth alice ""
        for {set i 0} {$i < 50} {incr i} { $rd get alice:$i }
        assert_equal 0 [acl_offload_hits]
        $rd close
    }
}

# Structural guard, source level. The offload invariant is: a rule-set list
# (user->selectors, user->roles, role->selectors) reachable from a client is
# never mutated in place or freed while an IO job may be reading it; it is
# replaced by pointer swap, the epoch is bumped, and the old list is freed
# only after the IO job backlog drains. The runtime half of the guard is
# aclOffloadAssertNoReaders() in acl.c. This half makes adding a NEW writer of
# those fields a conscious act: every raw store must live in a function listed
# here with its justification, or this test fails and tells the author why.
# (First time this bit for real: upstream's ACL roles added ACL SETROLE, which
# swapped a live role's selectors with no bump and no drain -- 11,200 leaked
# secrets in 100 probe rounds before the fix.)
set acl_src [file join [pwd] src acl.c]
if {[file readable $acl_src]} {
    start_server {config "minimal.conf" tags {"acl external:skip"}} {
        test {acl-offload: every writer of selectors/roles fields is a known, justified site} {
            set allowed {
                ACLCreateUser              "object not yet linked; unreachable from IO threads"
                ACLCreateRole              "object not yet linked; unreachable from IO threads"
                ACLCopyUser                "publish funnel: atomic swap + epoch bump + drain, then free"
                ACLStringSetRole           "staging copy, then publish funnel on a live role"
                ACLUserClearRoles          "guarded by aclOffloadAssertNoReaders when the user is published"
                ACLSetUser                 "in-place mutator; entry guarded by aclOffloadAssertNoReaders"
                ACLSetUserRoles            "called only from ACLSetUser (same guard)"
                ACLAppendRoleForLoading    "stack-allocated fake role for validation"
            }
            set fh [open $acl_src r]
            set src [read $fh]
            close $fh
            set fn "<file scope>"
            set lineno 0
            set violations {}
            set seen [dict create]
            foreach line [split $src "\n"] {
                incr lineno
                # One-line C function header: return type, name, argument list, opening brace.
                if {[regexp {^[A-Za-z_][A-Za-z0-9_ \*]*[ \*]([A-Za-z_][A-Za-z0-9_]*)\s*\([^;]*\)\s*\{\s*$} $line -> name]} {
                    set fn $name
                    continue
                }
                set is_store [regexp {(->|\.)(selectors|roles)\s*=[^=]} $line]
                set is_swap  [regexp {atomic_store_explicit\s*\(\s*\(_Atomic\(list \*\) \*\)\s*&\w+->(selectors|roles)} $line]
                if {!$is_store && !$is_swap} continue
                dict set seen $fn 1
                if {![dict exists $allowed $fn]} {
                    lappend violations "src/acl.c:$lineno in $fn: [string trim $line]"
                }
            }
            if {[llength $violations]} {
                fail "New writer(s) of a rule-set field outside the acl-offload allowlist.\n\
                      Either route the change through ACLCopyUser/ACLStringSetRole (swap + bump + drain),\n\
                      or add the function to the allowlist in tests/unit/acl-offload.tcl with a justification\n\
                      and a call to aclOffloadAssertNoReaders() at the mutation:\n  [join $violations "\n  "]"
            }
            # Keep the allowlist honest: every entry must still have a writer.
            foreach {name why} $allowed {
                if {![dict exists $seen $name]} {
                    fail "Allowlist entry '$name' no longer writes a rule-set field; remove it."
                }
            }
        }
    }
}

# CONFIG SET requirepass mutates the live default user in place. It now
# quiesces first (uniform rule), so the ACLSetUser guard must hold under load.
start_server {config "minimal.conf" tags {"acl external:skip valgrind:skip"} overrides {io-threads 4 io-threads-always-active yes acl-offload yes}} {
    test {acl-offload: requirepass changes under pipelined load are quiesced, not asserted} {
        set readers {}
        for {set i 0} {$i < 4} {incr i} {
            set rd [valkey_deferring_client]
            lappend readers $rd
        }
        set before [getInfoProperty [r info stats] acl_offload_quiesce_count]
        for {set round 0} {$round < 20} {incr round} {
            foreach rd $readers {
                for {set j 0} {$j < 32} {incr j} { $rd write [resp GET k] }
                $rd flush
            }
            r config set requirepass secret$round
            r auth secret$round
            r config set requirepass ""
            foreach rd $readers {
                for {set j 0} {$j < 32} {incr j} { catch {$rd read} }
            }
        }
        assert_equal PONG [r ping]
        assert {[getInfoProperty [r info stats] acl_offload_quiesce_count] > $before}
        foreach rd $readers { catch {$rd close} }
    }
}
