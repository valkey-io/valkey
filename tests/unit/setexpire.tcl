set ::live_member_ttl 100

proc flush_and_disable_active_expiry {} {
    r FLUSHALL
    r DEBUG SET-ACTIVE-EXPIRE 0
}

proc create_numbered_set {key n} {
    set members {}
    for {set i 0} {$i < $n} {incr i} { lappend members m$i }
    r SADD $key {*}$members
}

proc assert_none_of {got forbidden} {
    foreach m $got {
        assert_equal -1 [lsearch -exact $forbidden $m] "unexpected member $m"
    }
}

# Test members fit set-max-listpack-value, so this limit selects the encoding.
proc use_set_encoding {encoding {client r}} {
    switch $encoding {
        hashtable { $client config set set-max-listpack-entries 0 }
        listpack { $client config set set-max-listpack-entries 128 }
        default { error "unknown set encoding $encoding" }
    }
}

proc set_member_ttl {r key member} {
    return [lindex [$r STTL $key MEMBERS 1 $member] 0]
}

proc set_member_pexpiretime {r key member} {
    return [lindex [$r SPEXPIRETIME $key MEMBERS 1 $member] 0]
}

# Requires active expiry off, so the expired members stay in the set.
proc make_members_expired {r key members} {
    $r SPEXPIRE $key 1 MEMBERS [llength $members] {*}$members
    foreach member $members {
        wait_for_condition 100 10 {
            [$r SISMEMBER $key $member] == 0
        } else {
            fail "member $member of $key did not expire"
        }
    }
}

proc wait_for_set_active_expiry {r key expected_card initial_expired expected_increment} {
    wait_for_condition 100 100 {
        [$r SCARD $key] == $expected_card &&
        [status $r expired_set_members] == ($initial_expired + $expected_increment)
    } else {
        set got [status $r expired_set_members]
        set want [expr {$initial_expired + $expected_increment}]
        fail "Active expiry did not happen: SCARD [$r SCARD $key] (want $expected_card), expired_set_members $got (want $want)"
    }
}

# Return {events dirty_delta watch_fired}.
proc capture_mutation_signals {key body} {
    set rd [setup_single_keyspace_notification r]
    set watcher [valkey_client]
    $watcher watch $key
    $watcher multi
    $watcher ping

    set dirty [s rdb_changes_since_last_save]
    uplevel 1 $body
    set dirty_delta [expr {[s rdb_changes_since_last_save] - $dirty}]
    set watch_fired [expr {[$watcher exec] eq {}}]
    $watcher close

    # A marker event tells the reader where the events of $body end.
    r SADD mutation-marker x
    set events {}
    while 1 {
        set event [$rd read]
        set name [lindex [split [lindex $event 2] ":"] end]
        set event_key [lindex $event 3]
        if {$name eq "sadd" && $event_key eq "mutation-marker"} break
        lappend events "$name $event_key"
    }
    $rd close
    r DEL mutation-marker
    return [list $events $dirty_delta $watch_fired]
}

# The first byte of a DUMP payload is the RDB type.
proc dump_payload_type {payload} {
    binary scan [string index $payload 0] cu type
    return $type
}

# A SET_2 DUMP payload of member-expiry pairs, with a zero checksum that only
# DEBUG SET-SKIP-CHECKSUM-VALIDATION 1 accepts.
proc set2_payload {pairs} {
    set payload [binary format cc 24 [expr {[llength $pairs] / 2}]]
    foreach {member expiry} $pairs {
        append payload [binary format c [string length $member]] $member [binary format w $expiry]
    }
    append payload [binary format sw 82 0]
    return $payload
}

proc seed_expired_member {r key} {
    $r SADDEX $key EX $::live_member_ttl MEMBERS 1 live
    $r SADD $key expired
    make_members_expired $r $key expired
}

# Build a set whose expired members are hidden but not yet reclaimable. Above 127
# volatile members the expiry index is a RAX whose bucket key is the deadline
# rounded up to an 8192 ms window, and it reports that key as the earliest
# expiry, so for the rest of the window the index claims nothing has expired
# while every hidden member is still there. Returns the live member names.
proc numbered_members {prefix n} {
    set members {}
    for {set i 0} {$i < $n} {incr i} { lappend members $prefix$i }
    return $members
}

set ::rax_window_ms 8192

proc make_rax_window_set {r key hidden live} {
    assert_lessthan $hidden 127 "a full vector bucket re-keys its entries by their own windows"
    # Enter the window early enough that the deadline passes and the test body
    # runs before the window ends.
    while {[clock milliseconds] % $::rax_window_ms >= $::rax_window_ms / 2} { after 50 }

    set hidden_members [numbered_members h $hidden]
    set deadline [expr {[clock milliseconds] + 300}]
    # The index files the deadline under the end of the window containing it.
    set ::rax_window_end [expr {($deadline / $::rax_window_ms + 1) * $::rax_window_ms}]
    $r SADDEX $key PXAT $deadline MEMBERS [llength $hidden_members] {*}$hidden_members

    # A second, far-future window makes the index a genuine multi-bucket RAX:
    # the hidden members' near bucket is the one a census cannot settle from its
    # key alone, while this far bucket is wholly live and settleable from its
    # key. Padding to 128 volatile members forces the vector -> RAX conversion,
    # after which the same-deadline hidden members share one window bucket.
    set far_members [numbered_members far [expr {128 - $hidden}]]
    $r SADDEX $key PXAT [expr {$::rax_window_end + $::rax_window_ms}] MEMBERS [llength $far_members] {*}$far_members

    # Plain members first so callers can index the live list positionally; the
    # far members are live too and follow them.
    set plain_members [numbered_members live $live]
    $r SADD $key {*}$plain_members
    set live_members [concat $plain_members $far_members]

    wait_for_condition 100 10 {
        [$r SISMEMBER $key [lindex $hidden_members 0]] == 0 &&
        [$r SISMEMBER $key [lindex $hidden_members end]] == 0
    } else {
        fail "the hidden members of $key did not expire"
    }
    assert_equal [expr {$hidden + [llength $live_members]}] [$r SCARD $key]
    assert_equal [llength $live_members] [llength [$r SMEMBERS $key]]
    assert_inside_rax_window
    return $live_members
}

# Outside the window the index answers correctly and proves nothing, so a run
# that drifts past it must fail rather than pass for the wrong reason.
proc assert_inside_rax_window {} {
    set remaining [expr {$::rax_window_end - [clock milliseconds]}]
    assert_morethan $remaining 0 \
        "the expiry-index window ended [expr {-$remaining}] ms ago, so this run proves nothing"
}

# Every returned member must be one of the live ones the fixture created.
proc assert_all_live {got live_members} {
    foreach m $got {
        assert_not_equal -1 [lsearch -exact $live_members $m] "returned hidden member $m"
    }
}

# Build a hashtable set of $hidden expired members plus live members, $live_ttl
# with a TTL and $live_plain without. Returns the live member names.
proc make_mostly_expired_set {r key hidden live_ttl live_plain} {
    $r DEL $key
    set hidden_members {}
    for {set i 0} {$i < $hidden} {incr i} { lappend hidden_members h$i }
    $r SADD $key {*}$hidden_members
    make_members_expired $r $key $hidden_members

    set live_members {}
    for {set i 0} {$i < $live_ttl} {incr i} { lappend live_members t$i }
    if {$live_ttl > 0} { $r SADDEX $key EX 3600 MEMBERS $live_ttl {*}$live_members }
    set plain_members {}
    for {set i 0} {$i < $live_plain} {incr i} { lappend plain_members p$i }
    if {$live_plain > 0} { $r SADD $key {*}$plain_members }

    set live_members [concat $live_members $plain_members]
    assert_equal [expr {$hidden + $live_ttl + $live_plain}] [$r SCARD $key]
    assert_equal [llength $live_members] [llength [$r SMEMBERS $key]]
    return $live_members
}

# The primary stays populated so pre-sync setup is included in the full sync.
proc attach_replica {primary replica primary_host primary_port} {
    $replica replicaof $primary_host $primary_port
    wait_for_sync $replica
    wait_for_ofs_sync $primary $replica
}

# Hand $key's slot from node $from to node $to, moving the key with it.
proc migrate_key_slot {from to key} {
    set slot [R $from CLUSTER KEYSLOT $key]
    set from_id [R $from CLUSTER MYID]
    set to_id [R $to CLUSTER MYID]

    assert_equal {OK} [R $from CLUSTER SETSLOT $slot MIGRATING $to_id]
    assert_equal {OK} [R $to CLUSTER SETSLOT $slot IMPORTING $from_id]
    assert_equal {OK} [R $from MIGRATE [srv [expr {-$to}] host] [srv [expr {-$to}] port] $key 0 5000]
    assert_equal {OK} [R $to CLUSTER SETSLOT $slot NODE $to_id]
    assert_equal {OK} [R $from CLUSTER SETSLOT $slot NODE $to_id]
}

start_server {tags {"setexpire"}} {
    test {SEXPIRE - reply 1 when the TTL is applied} {
        r FLUSHALL
        r SADD myset a b c
        assert_equal {1 1 1} [r SEXPIRE myset $::live_member_ttl MEMBERS 3 a b c]
    }

    test {SEXPIRE - reply -2 for a missing key} {
        r FLUSHALL
        assert_equal {-2} [r SEXPIRE nosuchkey $::live_member_ttl MEMBERS 1 a]
        assert_equal 0 [r EXISTS nosuchkey]
    }

    test {SEXPIRE - reply -2 for a missing member of an existing key} {
        r FLUSHALL
        r SADD myset a b c
        assert_equal {1 -2} [r SEXPIRE myset $::live_member_ttl MEMBERS 2 a nomember]
        assert_equal 3 [r SCARD myset]
    }

    test {SEXPIRE - NX applies only to a member without a TTL} {
        r FLUSHALL
        r SADD myset a b
        assert_equal {1} [r SEXPIRE myset $::live_member_ttl MEMBERS 1 a]
        set original [set_member_pexpiretime r myset a]
        assert_equal {0 1} [r SEXPIRE myset $::live_member_ttl NX MEMBERS 2 a b]
        assert_equal $original [set_member_pexpiretime r myset a]
        assert_morethan [set_member_ttl r myset b] 0
    }

    test {SEXPIRE - XX applies only to a member that has a TTL} {
        r FLUSHALL
        r SADD myset a b
        assert_equal {1} [r SEXPIRE myset $::live_member_ttl MEMBERS 1 a]
        set original [set_member_pexpiretime r myset a]
        assert_equal {1 0} [r SEXPIRE myset [expr {2 * $::live_member_ttl}] XX MEMBERS 2 a b]
        assert_morethan [set_member_pexpiretime r myset a] $original
        assert_equal -1 [set_member_ttl r myset b]
    }

    test {SEXPIRE - GT applies only a greater expiration} {
        r FLUSHALL
        r SADD myset a
        assert_equal {1} [r SEXPIRE myset $::live_member_ttl MEMBERS 1 a]
        set original [set_member_pexpiretime r myset a]
        assert_equal {0} [r SEXPIRE myset 1 GT MEMBERS 1 a]
        assert_equal $original [set_member_pexpiretime r myset a]
        assert_equal {1} [r SEXPIRE myset [expr {2 * $::live_member_ttl}] GT MEMBERS 1 a]
        assert_morethan [set_member_pexpiretime r myset a] $original
    }

    test {SEXPIRE - LT applies only a smaller expiration} {
        r FLUSHALL
        r SADD myset a
        assert_equal {1} [r SEXPIRE myset $::live_member_ttl MEMBERS 1 a]
        set original [set_member_pexpiretime r myset a]
        assert_equal {0} [r SEXPIRE myset [expr {2 * $::live_member_ttl}] LT MEMBERS 1 a]
        assert_equal $original [set_member_pexpiretime r myset a]
        assert_equal {1} [r SEXPIRE myset 1 LT MEMBERS 1 a]
        assert_lessthan [set_member_pexpiretime r myset a] $original
    }

    test {SEXPIRE - GT and LT treat a member with no TTL as infinite} {
        r FLUSHALL
        r SADD myset a b
        assert_equal {0} [r SEXPIRE myset $::live_member_ttl GT MEMBERS 1 a]
        assert_equal -1 [set_member_ttl r myset a]
        assert_equal {1} [r SEXPIRE myset $::live_member_ttl LT MEMBERS 1 b]
        assert_morethan [set_member_ttl r myset b] 0
    }

    test {SEXPIRE - reply 2 and member removal for a time in the past} {
        r FLUSHALL
        r SADD myset a b c
        assert_equal {2} [r SEXPIRE myset 0 MEMBERS 1 a]
        assert_equal -2 [set_member_ttl r myset a]
        assert_equal {b c} [lsort [r SMEMBERS myset]]
    }

    test {SEXPIRE - the key is deleted when a past expiration removes the last member} {
        r FLUSHALL
        r SADD myset a
        assert_equal {2} [r SEXPIRE myset 0 MEMBERS 1 a]
        assert_equal 0 [r EXISTS myset]
        assert_equal 0 [get_keys_with_volatile_items r]
    }

    test {SEXPIRE - a negative relative TTL is rejected} {
        r FLUSHALL
        r SADD myset a
        assert_error "*invalid expire time*" {r SEXPIRE myset -10 MEMBERS 1 a}
        assert_equal 1 [r SISMEMBER myset a]
    }

    test {SEXPIRE - duplicate members get one reply element each} {
        r FLUSHALL
        r SADD myset a
        assert_equal {1 1} [r SEXPIRE myset $::live_member_ttl MEMBERS 2 a a]
    }

    test {SEXPIRE - WRONGTYPE against a non-set key} {
        r FLUSHALL
        r SET mystr hello
        assert_error "WRONGTYPE*" {r SEXPIRE mystr $::live_member_ttl MEMBERS 1 a}
    }

    test {SEXPIRE - MEMBERS count validation} {
        r FLUSHALL
        r SADD myset a b c
        assert_error "*greater than 0 and match the provided number*" \
            {r SEXPIRE myset $::live_member_ttl MEMBERS 2 a}
        assert_error "*greater than 0 and match the provided number*" \
            {r SEXPIRE myset $::live_member_ttl MEMBERS 0 a}
        assert_error "*not an integer or out of range" \
            {r SEXPIRE myset $::live_member_ttl MEMBERS notanumber a}
    }

    test {SPEXPIRE - the argument is relative milliseconds} {
        r FLUSHALL
        r SADD myset a
        set before_ms [clock milliseconds]
        assert_equal {1} [r SPEXPIRE myset 100000 MEMBERS 1 a]
        set after_ms [clock milliseconds]
        assert_range [set_member_pexpiretime r myset a] \
            [expr {$before_ms + 100000}] [expr {$after_ms + 100000}]
    }

    test {SEXPIREAT - the argument is an absolute unix time in seconds} {
        r FLUSHALL
        r SADD myset a
        set at_s [expr {[clock seconds] + 100}]
        assert_equal {1} [r SEXPIREAT myset $at_s MEMBERS 1 a]
        assert_equal [expr {$at_s * 1000}] [set_member_pexpiretime r myset a]
    }

    test {SPEXPIREAT - the argument is an absolute unix time in milliseconds} {
        r FLUSHALL
        r SADD myset a
        set at_ms [expr {[clock milliseconds] + 100000}]
        assert_equal {1} [r SPEXPIREAT myset $at_ms MEMBERS 1 a]
        assert_equal $at_ms [set_member_pexpiretime r myset a]
    }

    test {STTL - mixed -2, -1 and value replies keep member order} {
        r FLUSHALL
        assert_equal {-2} [r STTL nosuchkey MEMBERS 1 a]
        r SADD myset a b
        assert_equal {1} [r SEXPIRE myset $::live_member_ttl MEMBERS 1 a]
        set res [r STTL myset MEMBERS 3 a b nomember]
        assert_equal 3 [llength $res]
        assert_morethan [lindex $res 0] 0
        assert_equal -1 [lindex $res 1]
        assert_equal -2 [lindex $res 2]
    }

    test {STTL and SPTTL - value ranges after a relative expiration} {
        r FLUSHALL
        r SADD myset a
        assert_equal {1} [r SEXPIRE myset 100 MEMBERS 1 a]
        assert_range [set_member_ttl r myset a] 90 100
        assert_range [lindex [r SPTTL myset MEMBERS 1 a] 0] 90000 100000
    }

    test {SEXPIRETIME rounds to the nearest second} {
        r FLUSHALL
        r SADD myset a
        # A .7s stamp, so the rounded second differs from the truncated one.
        set at_ms [expr {([clock milliseconds] + 100000) / 1000 * 1000 + 700}]
        assert_equal {1} [r SPEXPIREAT myset $at_ms MEMBERS 1 a]
        assert_equal [expr {$at_ms / 1000 + 1}] [lindex [r SEXPIRETIME myset MEMBERS 1 a] 0]
    }

    test {Set TTL readers - WRONGTYPE against a non-set key} {
        r FLUSHALL
        r SET mystr hello
        assert_error "WRONGTYPE*" {r STTL mystr MEMBERS 1 a}
    }

    test {Set TTL readers - MEMBERS count validation} {
        r FLUSHALL
        r SADD myset a b
        foreach reader {STTL SPTTL SEXPIRETIME SPEXPIRETIME} {
            # HTTL replies a syntax error too; the write commands reply the count error.
            assert_error "*ERR syntax error" {r $reader myset MEMBERS 1 a b}
            assert_error "*ERR syntax error" {r $reader myset MEMBERS 0 a}
            assert_error "*not an integer or out of range" {r $reader myset MEMBERS a b}
            assert_error "*ERR syntax error" {r $reader myset FIELDS 1 a}
        }
    }

    test {SPERSIST - -2 for a missing key} {
        r FLUSHALL
        assert_equal {-2} [r SPERSIST nosuchkey MEMBERS 1 a]
    }

    test {SPERSIST - 1 when a TTL is removed and STTL reports -1 afterwards} {
        r FLUSHALL
        r SADD myset a b c
        assert_equal {1 1} [r SEXPIRE myset $::live_member_ttl MEMBERS 2 a b]
        assert_equal {1} [r SPERSIST myset MEMBERS 1 a]
        assert_equal -1 [set_member_ttl r myset a]
        assert_morethan [set_member_ttl r myset b] 0
        assert_equal 3 [r SCARD myset]
    }

    test {SPERSIST - mixed replies keep member order} {
        r FLUSHALL
        r SADD myset a b
        assert_equal {1} [r SEXPIRE myset $::live_member_ttl MEMBERS 1 a]
        assert_equal {1 -1 -2} [r SPERSIST myset MEMBERS 3 a b nomember]
    }

    test {SPERSIST - the key stops being tracked when its last TTL is removed} {
        r FLUSHALL
        r SADD myset a b
        assert_equal {1 1} [r SEXPIRE myset $::live_member_ttl MEMBERS 2 a b]
        assert_equal 1 [get_keys_with_volatile_items r]
        assert_equal {1} [r SPERSIST myset MEMBERS 1 a]
        assert_equal 1 [get_keys_with_volatile_items r]
        assert_equal {1} [r SPERSIST myset MEMBERS 1 b]
        assert_equal 0 [get_keys_with_volatile_items r]
    }

    test {SPERSIST - WRONGTYPE and argument errors} {
        r FLUSHALL
        r SET mystr hello
        r SADD myset a b
        assert_error "WRONGTYPE*" {r SPERSIST mystr MEMBERS 1 a}
        assert_error "*ERR syntax error" {r SPERSIST myset a 1 b}
        assert_error "*not an integer or out of range" {r SPERSIST myset MEMBERS a b}
        assert_error "*greater than 0 and match the provided number*" \
            {r SPERSIST myset MEMBERS 3 a b}
        assert_error "*greater than 0 and match the provided number*" \
            {r SPERSIST myset MEMBERS 0 a}
    }

    test {SADDEX EX - the reply is 1 and the TTL is set} {
        r FLUSHALL
        assert_equal 1 [r SADDEX myset EX $::live_member_ttl MEMBERS 2 a b]
        assert_range [set_member_ttl r myset a] 90 $::live_member_ttl
        assert_range [set_member_ttl r myset b] 90 $::live_member_ttl
        assert_equal 1 [get_keys_with_volatile_items r]
    }

    test {SADDEX EX - an existing member's TTL is updated and the reply is still 1} {
        r FLUSHALL
        r SADD myset a
        assert_equal 1 [r SADDEX myset EX $::live_member_ttl MEMBERS 2 a b]
        assert_range [set_member_ttl r myset a] 90 $::live_member_ttl
        assert_equal 1 [r SADDEX myset EX [expr {2 * $::live_member_ttl}] MEMBERS 2 a b]
        foreach member {a b} {
            assert_range [set_member_ttl r myset $member] 190 [expr {2 * $::live_member_ttl}]
        }
    }

    test {SADDEX EX - an existing intset member receives the TTL} {
        r FLUSHALL
        r SADD myset 1
        assert_encoding intset myset
        assert_equal 1 [r SADDEX myset EX $::live_member_ttl MEMBERS 1 1]
        assert_encoding listpack myset
        assert_morethan [set_member_ttl r myset 1] 0
    }

    # HSETEX also replies 1 here, including over a missing key.
    test {SADDEX EX - a past expiration removes existing members, adds none and replies 1} {
        r FLUSHALL
        assert_equal 1 [r SADDEX myset EX 0 MEMBERS 1 a]
        assert_equal 0 [r EXISTS myset]
        r SADD myset a
        assert_equal 1 [r SADDEX myset EX 0 MEMBERS 2 a b]
        assert_equal 0 [r EXISTS myset]
    }

    foreach {expiration value reader} [list PX 100000000 SPTTL \
            EXAT [expr {[clock seconds] + 100000}] SEXPIRETIME \
            PXAT [expr {[clock milliseconds] + 100000000}] SPEXPIRETIME] {
        test "SADDEX $expiration - the expiration is read back in its own unit and frame" {
            r FLUSHALL
            assert_equal 1 [r SADDEX myset $expiration $value MEMBERS 1 a]
            if {[string match "*AT" $expiration]} {
                assert_equal $value [lindex [r $reader myset MEMBERS 1 a] 0]
            } else {
                assert_range [lindex [r $reader myset MEMBERS 1 a] 0] \
                    [expr {$value - 5000}] $value
            }
        }
    }

    test {SADDEX - without an expiration token the members are plain} {
        r FLUSHALL
        assert_equal 1 [r SADDEX myset MEMBERS 2 a b]
        assert_equal {-1 -1} [r STTL myset MEMBERS 2 a b]
        assert_equal 0 [get_keys_with_volatile_items r]
    }

    test {SADDEX KEEPTTL - keeps an existing member TTL and adds new members plain} {
        r FLUSHALL
        assert_equal 1 [r SADDEX myset EX $::live_member_ttl MEMBERS 1 a]
        set original [set_member_pexpiretime r myset a]
        assert_equal 1 [r SADDEX myset KEEPTTL MEMBERS 2 a b]
        assert_equal $original [set_member_pexpiretime r myset a]
        assert_equal -1 [set_member_ttl r myset b]
    }

    test {SADDEX MNX EX - all or nothing over the whole call} {
        r FLUSHALL
        r SADD myset a
        assert_equal 0 [r SADDEX myset MNX EX $::live_member_ttl MEMBERS 2 a b]
        assert_equal -1 [set_member_ttl r myset a]
        assert_equal {a} [r SMEMBERS myset]
        assert_equal 1 [r SADDEX myset MNX EX $::live_member_ttl MEMBERS 2 c d]
        assert_equal {a c d} [lsort [r SMEMBERS myset]]
        assert_morethan [set_member_ttl r myset c] 0
        assert_morethan [set_member_ttl r myset d] 0
    }

    # 1 when the TTLs are applied, 0 when the condition fails, though neither adds a member.
    test {SADDEX MXX EX - all or nothing over the whole call} {
        r FLUSHALL
        assert_equal 0 [r SADDEX nokey MXX EX $::live_member_ttl MEMBERS 1 a]
        assert_equal 0 [r EXISTS nokey]
        r SADD myset a
        assert_equal 0 [r SADDEX myset MXX EX $::live_member_ttl MEMBERS 2 a b]
        assert_equal -1 [set_member_ttl r myset a]
        assert_equal {a} [r SMEMBERS myset]
        r SADD myset b
        assert_equal 1 [r SADDEX myset MXX EX $::live_member_ttl MEMBERS 2 a b]
        assert_morethan [set_member_ttl r myset a] 0
        assert_morethan [set_member_ttl r myset b] 0
    }

    # A repeated member must receive the one expiration change, not one per repeat.
    test {SADDEX MXX - a repeated member receives the TTL once} {
        r FLUSHALL
        use_set_encoding hashtable
        r SADD myset member
        assert_equal 1 [r SADDEX myset MXX EX $::live_member_ttl MEMBERS 2 member member]
        assert_equal 1 [r SCARD myset]
        assert_morethan [set_member_ttl r myset member] 0
    }

    test {SADDEX MXX without an expiration - a repeated volatile member becomes plain} {
        r FLUSHALL
        use_set_encoding hashtable
        assert_equal 1 [r SADDEX myset EX $::live_member_ttl MEMBERS 1 member]
        assert_equal 1 [r SADDEX myset MXX MEMBERS 2 member member]
        assert_equal 1 [r SCARD myset]
        assert_equal -1 [set_member_ttl r myset member]
        use_set_encoding listpack
    } {OK}

    test {SADDEX NX - the key condition is honoured} {
        r FLUSHALL
        assert_equal 1 [r SADDEX myset NX EX $::live_member_ttl MEMBERS 1 a]
        assert_equal 1 [r EXISTS myset]
        assert_morethan [set_member_ttl r myset a] 0
        assert_equal 0 [r SADDEX myset NX EX $::live_member_ttl MEMBERS 1 b]
        assert_equal 0 [r SISMEMBER myset b]
    }

    test {SADDEX XX - the key condition is honoured} {
        r FLUSHALL
        assert_equal 0 [r SADDEX myset XX EX $::live_member_ttl MEMBERS 1 a]
        assert_equal 0 [r EXISTS myset]
        r SADD myset a
        assert_equal 1 [r SADDEX myset XX EX $::live_member_ttl MEMBERS 1 b]
        assert_morethan [set_member_ttl r myset b] 0
    }

    test {SADDEX - WRONGTYPE and argument errors} {
        r FLUSHALL
        r SET mystr hello
        assert_error "WRONGTYPE*" {r SADDEX mystr EX 100 MEMBERS 1 a}
        assert_error "*ERR syntax error" {r SADDEX myset EX 100 PX 1000 MEMBERS 1 a}
        assert_error "*ERR syntax error" {r SADDEX myset MNX MXX EX 100 MEMBERS 1 a}
        assert_error "*ERR syntax error" {r SADDEX myset NX XX EX 100 MEMBERS 1 a}
        assert_error "*greater than 0 and match the provided number*" \
            {r SADDEX myset EX 100 MEMBERS 2 a}
        assert_error "*greater than 0 and match the provided number*" \
            {r SADDEX myset EX 100 MEMBERS 0 a}
        assert_error "*not an integer or out of range" {r SADDEX myset EX notanumber MEMBERS 1 a}
    }

    test {Set TTL commands - too few arguments} {
        r FLUSHALL
        r SADD myset a
        assert_error "ERR wrong number of arguments for 'sexpire' command" \
            {r SEXPIRE myset 100 1 a}
        foreach command {STTL SPERSIST} {
            assert_error "ERR wrong number of arguments for '[string tolower $command]' command" \
                {r $command myset 1 a}
        }
    }

    test "SINTER SUNION SDIFF SINTERCARD never count an expired member" {
        flush_and_disable_active_expiry
        r SADD s1{t} a b c d
        r SADD s2{t} a b c e
        make_members_expired r s1{t} {b d}
        make_members_expired r s2{t} {c}
        assert_equal {a} [lsort [r SINTER s1{t} s2{t}]]
        assert_equal {a b c e} [lsort [r SUNION s1{t} s2{t}]]
        assert_equal {c} [lsort [r SDIFF s1{t} s2{t}]]
        assert_equal 1 [r SINTERCARD 2 s1{t} s2{t}]
        # A single-key SINTERCARD is the live cardinality, not SCARD.
        assert_equal 2 [r SINTERCARD 1 s1{t}]
        assert_equal 4 [r SCARD s1{t}]
        assert_equal 4 [r SCARD s2{t}]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test "SUNIONSTORE writes a plain set when a source is volatile" {
        flush_and_disable_active_expiry
        r SADD s1{t} a b c
        r SADD s2{t} d e
        make_members_expired r s1{t} {c}
        assert_equal {1} [r SEXPIRE s1{t} $::live_member_ttl MEMBERS 1 a]
        assert_equal 4 [r SUNIONSTORE dst{t} s1{t} s2{t}]
        assert_equal {a b d e} [lsort [r SMEMBERS dst{t}]]
        assert_equal {-1 -1 -1 -1} [r STTL dst{t} MEMBERS 4 a b d e]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test "SMOVE carries the member TTL to the destination" {
        r FLUSHALL
        r SADD src{t} a b
        r SADD dst{t} z
        assert_equal {1} [r SEXPIRE src{t} $::live_member_ttl MEMBERS 1 a]
        set expiry [set_member_pexpiretime r src{t} a]
        assert_equal 1 [r SMOVE src{t} dst{t} a]
        assert_equal $expiry [set_member_pexpiretime r dst{t} a]
    }

    test "SMOVE of a plain member into a volatile destination keeps it plain" {
        r FLUSHALL
        r SADD src{t} a
        r SADD dst{t} z y
        assert_equal {1} [r SEXPIRE dst{t} $::live_member_ttl MEMBERS 1 z]
        assert_equal 1 [r SMOVE src{t} dst{t} a]
        assert_equal {-1} [r STTL dst{t} MEMBERS 1 a]
    }

    test "SMOVE of an expired member returns 0 and leaves the destination alone" {
        flush_and_disable_active_expiry
        r SADD src{t} a b
        r SADD dst{t} z
        make_members_expired r src{t} {a}
        assert_equal 0 [r SMOVE src{t} dst{t} a]
        assert_equal {z} [r SMEMBERS dst{t}]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test "SPOP removes only the live member it returns" {
        flush_and_disable_active_expiry
        r SADD myset a b c
        make_members_expired r myset {a b c}
        r SADD myset live1 live2

        set signals [capture_mutation_signals myset {
            set popped [r SPOP myset]
        }]
        assert_all_live [list $popped] {live1 live2}
        assert_equal {{{spop myset}} 1 1} $signals
        assert_equal 4 [r SCARD myset]

        set signals [capture_mutation_signals myset {
            set popped [r SPOP myset 1]
        }]
        assert_all_live [list $popped] {live1 live2}
        assert_equal {{{spop myset}} 1 1} $signals
        assert_equal 3 [r SCARD myset]
        assert_equal {} [r SMEMBERS myset]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    foreach command {SINTERSTORE SUNIONSTORE SDIFFSTORE} {
        test "$command over only expired members raises no mutation signal" {
            flush_and_disable_active_expiry
            r SADD s1{t} a b
            make_members_expired r s1{t} {a b}
            assert_equal {{} 0 0} [capture_mutation_signals dst{t} {
                assert_equal 0 [r $command dst{t} s1{t}]
            }]
            assert_equal 0 [r EXISTS dst{t}]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}
    }

    test "SORT STORE over only expired members raises no mutation signal" {
        flush_and_disable_active_expiry
        r SADD src{t} 1 2 3
        make_members_expired r src{t} {1 2 3}
        assert_equal {{} 0 0} [capture_mutation_signals dst{t} {
            assert_equal 0 [r SORT src{t} STORE dst{t}]
        }]
        assert_equal 0 [r EXISTS dst{t}]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {A member TTL does not change a listpack set's encoding} {
        r FLUSHALL
        use_set_encoding listpack
        r SADD myset a b c
        assert_encoding listpack myset
        assert_equal {1} [r SEXPIRE myset $::live_member_ttl MEMBERS 1 a]
        assert_encoding listpack myset
        assert_equal {1} [r SEXPIRE myset [expr {2 * $::live_member_ttl}] MEMBERS 1 a]
        assert_encoding listpack myset
        assert_equal {1} [r SPERSIST myset MEMBERS 1 a]
        assert_encoding listpack myset
        assert_equal 1 [r SADDEX myset EX $::live_member_ttl MEMBERS 1 d]
        assert_encoding listpack myset
    }

    # Active expiry is disabled to expose logically expired members.
    foreach encoding {listpack hashtable} {
        test "SADD over an expired member returns 1 and clears the TTL - $encoding" {
            flush_and_disable_active_expiry
            r config resetstat
            use_set_encoding $encoding
            r SADD myset gone live1
            make_members_expired r myset {gone}
            set rd [setup_single_keyspace_notification r]
            assert_equal 1 [r SADD myset gone]
            assert_keyevent_patterns $rd myset sexpired sadd
            assert_equal 1 [status r expired_set_members]
            r SADD dummy dummy
            assert_keyevent_patterns $rd dummy sadd
            $rd close
            assert_equal -1 [set_member_ttl r myset gone]
            assert_equal 2 [r SCARD myset]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "SEXPIRE on an expired member replies -2 and does not resurrect it - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            r SADD myset gone live1
            make_members_expired r myset {gone}
            assert_equal {-2} [r SEXPIRE myset $::live_member_ttl MEMBERS 1 gone]
            assert_equal 0 [r SISMEMBER myset gone]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}
    }

    test "SADD over a live volatile member returns 0 and keeps the TTL" {
        r FLUSHALL
        r SADD myset a
        assert_equal {1} [r SEXPIRE myset $::live_member_ttl MEMBERS 1 a]
        set original [set_member_pexpiretime r myset a]
        assert_equal 0 [r SADD myset a]
        assert_equal $original [set_member_pexpiretime r myset a]
    }

    test "SADDEX MNX over an expired member treats it as absent" {
        flush_and_disable_active_expiry
        r SADD myset gone live1
        make_members_expired r myset {gone}
        assert_equal 1 [r SADDEX myset MNX EX $::live_member_ttl MEMBERS 1 gone]
        assert_morethan [set_member_ttl r myset gone] 0
        assert_equal 2 [r SCARD myset]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test "SADDEX MXX over an expired member treats it as absent" {
        flush_and_disable_active_expiry
        r SADD myset gone live1
        make_members_expired r myset {gone}
        assert_equal 0 [r SADDEX myset MXX EX $::live_member_ttl MEMBERS 2 gone live1]
        assert_equal 0 [r SISMEMBER myset gone]
        assert_equal -1 [set_member_ttl r myset live1]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {SEXPIRE emits a single sexpire notification} {
        r FLUSHALL
        r SADD myset a b c
        set rd [setup_single_keyspace_notification r]
        r SEXPIRE myset $::live_member_ttl MEMBERS 3 a b c
        assert_keyevent_patterns $rd myset sexpire
        # Nothing else may be pending: the next event is the marker.
        r SADD dummy dummy
        assert_keyevent_patterns $rd dummy sadd
        $rd close
    }

    test {SEXPIRE on a missing member emits nothing} {
        r FLUSHALL
        r SADD myset a
        set rd [setup_single_keyspace_notification r]
        r SEXPIRE myset $::live_member_ttl MEMBERS 1 nomember
        r SADD dummy dummy
        assert_keyevent_patterns $rd dummy sadd
        $rd close
    }

    test {A past SEXPIRE emits sexpired and del and increments expired_set_members} {
        r FLUSHALL
        r config resetstat
        r SADD myset a
        set rd [setup_single_keyspace_notification r]
        assert_equal {2} [r SEXPIRE myset 0 MEMBERS 1 a]
        assert_keyevent_patterns $rd myset sexpired del
        assert_equal 1 [status r expired_set_members]
        r SADD dummy dummy
        assert_keyevent_patterns $rd dummy sadd
        $rd close
    }

    test {SPERSIST emits an spersist notification} {
        r FLUSHALL
        r SADD myset a
        r SEXPIRE myset $::live_member_ttl MEMBERS 1 a
        set rd [setup_single_keyspace_notification r]
        r SPERSIST myset MEMBERS 1 a
        assert_keyevent_patterns $rd myset spersist
        r SADD dummy dummy
        assert_keyevent_patterns $rd dummy sadd
        $rd close
    }

    test {SPERSIST on a member without a TTL emits nothing} {
        r FLUSHALL
        r SADD myset a
        set rd [setup_single_keyspace_notification r]
        r SPERSIST myset MEMBERS 1 a
        r SADD dummy dummy
        assert_keyevent_patterns $rd dummy sadd
        $rd close
    }

    test {SADDEX emits sadd and sexpire} {
        r FLUSHALL
        set rd [setup_single_keyspace_notification r]
        r SADDEX myset EX $::live_member_ttl MEMBERS 1 a
        assert_keyevent_patterns $rd myset sadd sexpire
        r SADD dummy dummy
        assert_keyevent_patterns $rd dummy sadd
        $rd close
    }

    # HSETEX emits only hset when it clears a field's TTL.
    test {SADDEX without an expiration emits only sadd when it clears a member TTL} {
        r FLUSHALL
        r SADDEX myset EX $::live_member_ttl MEMBERS 1 a
        set rd [setup_single_keyspace_notification r]
        assert_equal 1 [r SADDEX myset MEMBERS 1 a]
        assert_equal -1 [set_member_ttl r myset a]
        assert_keyevent_patterns $rd myset sadd
        r SADD dummy dummy
        assert_keyevent_patterns $rd dummy sadd
        $rd close
    }

    # As HSETEX emits hset, a call that passed its checks emits sadd even when it changes nothing.
    test {SADDEX that changes nothing still emits sadd} {
        r FLUSHALL
        r SADD myset a
        set rd [setup_single_keyspace_notification r]
        assert_equal 1 [r SADDEX myset MEMBERS 1 a]
        assert_equal 1 [r SADDEX myset KEEPTTL MEMBERS 1 a]
        assert_keyevent_patterns $rd myset sadd sadd
        r SADD dummy dummy
        assert_keyevent_patterns $rd dummy sadd
        $rd close
    }

    test {SADDEX whose condition fails emits nothing} {
        r FLUSHALL
        r SADD myset a
        set rd [setup_single_keyspace_notification r]
        assert_equal 0 [r SADDEX myset NX EX $::live_member_ttl MEMBERS 1 b]
        assert_equal 0 [r SADDEX nokey XX EX $::live_member_ttl MEMBERS 1 b]
        assert_equal 0 [r SADDEX myset MNX EX $::live_member_ttl MEMBERS 2 a b]
        assert_equal 0 [r SADDEX myset MXX EX $::live_member_ttl MEMBERS 2 a b]
        r SADD dummy dummy
        assert_keyevent_patterns $rd dummy sadd
        $rd close
    }

    foreach {expiration past} [list EX 0 PX 0 EXAT [expr {[clock seconds] - 200000}] \
            PXAT [expr {[clock milliseconds] - 200000}]] {
        test "SADDEX $expiration with a past time emits sadd, sexpire, sexpired and del" {
            r FLUSHALL
            r SADD myset a
            set rd [setup_single_keyspace_notification r]
            assert_equal 1 [r SADDEX myset $expiration $past MEMBERS 2 a b]
            assert_keyevent_patterns $rd myset sadd sexpire sexpired del
            r SADD dummy dummy
            assert_keyevent_patterns $rd dummy sadd
            $rd close
            assert_equal 0 [r EXISTS myset]
            assert_equal 0 [get_keys_with_volatile_items r]
        }
    }

    # HSETEX creates the key and deletes it again, emitting the same sequence.
    test {SADDEX with a past time over a missing key emits sadd, sexpire, sexpired and del} {
        r FLUSHALL
        set rd [setup_single_keyspace_notification r]
        assert_equal 1 [r SADDEX myset PXAT 1 MEMBERS 1 a]
        assert_keyevent_patterns $rd myset sadd sexpire sexpired del
        r SADD dummy dummy
        assert_keyevent_patterns $rd dummy sadd
        $rd close
        assert_equal 0 [r EXISTS myset]
    }

    # HSETEX emits hexpired first when it overwrites an expired field.
    foreach encoding {listpack hashtable} {
        test "SADDEX over an expired member emits sexpired before sadd and sexpire - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            r SADD myset gone live1
            make_members_expired r myset {gone}
            set rd [setup_single_keyspace_notification r]
            assert_equal 1 [r SADDEX myset EX $::live_member_ttl MEMBERS 1 gone]
            assert_keyevent_patterns $rd myset sexpired sadd sexpire
            r SADD dummy dummy
            assert_keyevent_patterns $rd dummy sadd
            $rd close
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}
    }

    foreach encoding {listpack hashtable} {
        test "Active expiry emits sexpired and increments expired_set_members - $encoding" {
            r FLUSHALL
            r config resetstat
            use_set_encoding $encoding
            r SADD myset a b c live1
            set rd [setup_single_keyspace_notification r]
            r SPEXPIRE myset 50 MEMBERS 3 a b c
            assert_keyevent_patterns $rd myset sexpire
            wait_for_set_active_expiry r myset 1 0 3
            assert_keyevent_patterns $rd myset sexpired
            r SADD dummy dummy
            assert_keyevent_patterns $rd dummy sadd
            $rd close
        }

        test "Active expiry of the last member deletes the key - $encoding" {
            r FLUSHALL
            r config resetstat
            use_set_encoding $encoding
            r SADD myset a
            set rd [setup_single_keyspace_notification r]
            r SPEXPIRE myset 50 MEMBERS 1 a
            assert_keyevent_patterns $rd myset sexpire
            wait_for_set_active_expiry r myset 0 0 1
            assert_keyevent_patterns $rd myset sexpired del
            assert_equal 0 [get_keys_with_volatile_items r]
            r SADD dummy dummy
            assert_keyevent_patterns $rd dummy sadd
            $rd close
        }
    }
    # Member TTL metadata forces intsets to convert; set mutations never convert
    # them back.
    test "A member TTL converts a small intset to listpack" {
        r FLUSHALL
        use_set_encoding listpack
        r SADD myset 1 2 3
        assert_encoding intset myset
        assert_equal {1} [r SEXPIRE myset $::live_member_ttl MEMBERS 1 2]
        assert_encoding listpack myset
        assert_equal {1 2 3} [lsort [r SMEMBERS myset]]
        assert_range [set_member_ttl r myset 2] 1 $::live_member_ttl
        assert_equal {-1 -1} [r STTL myset MEMBERS 2 1 3]
    }

    test "A member TTL converts an intset over the listpack limit to hashtable" {
        r FLUSHALL
        use_set_encoding listpack
        set ints {}
        for {set i 0} {$i < 200} {incr i} { lappend ints $i }
        r SADD myset {*}$ints
        assert_encoding intset myset
        assert_equal {1} [r SEXPIRE myset $::live_member_ttl MEMBERS 1 7]
        assert_encoding hashtable myset
        assert_equal 200 [r SCARD myset]
    }

    test "SPERSIST of the last volatile member keeps the converted encoding" {
        r FLUSHALL
        use_set_encoding listpack
        r SADD myset 1 2 3
        assert_encoding intset myset
        assert_equal {1} [r SEXPIRE myset $::live_member_ttl MEMBERS 1 2]
        assert_encoding listpack myset

        assert_equal {1} [r SPERSIST myset MEMBERS 1 2]
        assert_encoding listpack myset
        assert_equal {1 2 3} [lsort [r SMEMBERS myset]]
        assert_equal {-1 -1 -1} [r STTL myset MEMBERS 3 1 2 3]
    }

    test "A TTL command that applies no TTL keeps the intset encoding" {
        r FLUSHALL
        use_set_encoding listpack
        r SADD myset 1 2 3
        assert_encoding intset myset
        assert_equal {-2} [r SEXPIRE myset $::live_member_ttl MEMBERS 1 9]
        assert_encoding intset myset
        assert_equal {2} [r SEXPIRE myset 0 MEMBERS 1 2]
        assert_equal {1 3} [lsort [r SMEMBERS myset]]
        assert_encoding intset myset
    }

    test "SMOVE of a volatile integer member converts an intset destination" {
        r FLUSHALL
        use_set_encoding listpack
        r SADD src{t} 7
        r SADD dst{t} 1 2 3
        assert_encoding intset dst{t}
        assert_equal {1} [r SEXPIRE src{t} $::live_member_ttl MEMBERS 1 7]
        assert_equal 1 [r SMOVE src{t} dst{t} 7]
        assert_encoding listpack dst{t}
        assert_range [set_member_ttl r dst{t} 7] 1 $::live_member_ttl
        assert_equal {1 2 3 7} [lsort [r SMEMBERS dst{t}]]
        assert_equal {-1 -1 -1} [r STTL dst{t} MEMBERS 3 1 2 3]
    }

    test {Listpack to hashtable conversion keeps an expired member} {
        flush_and_disable_active_expiry
        r CONFIG SET set-max-listpack-entries 4
        r SADD myset expired l1 l2
        make_members_expired r myset {expired}
        assert_encoding listpack myset

        r SADD myset l3 l4 l5
        assert_encoding hashtable myset
        assert_equal 6 [r SCARD myset]
        assert_equal 0 [r SISMEMBER myset expired]

        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    foreach encoding {listpack hashtable} {
        test "SPOP with a count above the live size returns exactly the live members - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            create_numbered_set myset 10
            make_members_expired r myset {m0 m1 m2 m3}
            assert_equal 10 [r SCARD myset]

            set popped [r SPOP myset 8]
            assert_equal {m4 m5 m6 m7 m8 m9} [lsort $popped]
            assert_equal 4 [r SCARD myset]
            assert_equal {} [r SMEMBERS myset]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        # A listpack replies integer members through their own branches.
        test "SPOP and SRANDMEMBER return the live integer members - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            r SADD myset 1 2 3 4 5 6 7 8
            make_members_expired r myset {1 2 3 4 5 6}
            assert_encoding $encoding myset
            for {set i 0} {$i < 20} {incr i} {
                set got [r SRANDMEMBER myset 1]
                assert_equal 1 [llength $got]
                assert_all_live $got {7 8}
                set got [r SRANDMEMBER myset -3]
                assert_equal 3 [llength $got]
                assert_all_live $got {7 8}
            }
            assert_equal {7 8} [lsort [r SRANDMEMBER myset 5]]
            assert_equal {7 8} [lsort [r SPOP myset 5]]
            assert_equal 6 [r SCARD myset]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "SPOP with a count below the live size returns live members only - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            create_numbered_set myset 10
            make_members_expired r myset {m0 m1 m2 m3}
            set popped [r SPOP myset 3]
            assert_equal 3 [llength $popped]
            assert_equal 3 [llength [lsort -unique $popped]]
            assert_none_of $popped {m0 m1 m2 m3}
            assert_equal 7 [r SCARD myset]
            assert_equal 3 [llength [r SMEMBERS myset]]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "SPOP on a set of only expired members mutates nothing - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            r SADD myset a b c
            make_members_expired r myset {a b c}
            assert_equal 3 [r SCARD myset]
            assert_equal {{} 0 0} [capture_mutation_signals myset {
                assert_equal {} [r SPOP myset 2]
                assert_equal {} [r SPOP myset]
                assert_equal {} [r SPOP myset 3]
            }]
            assert_equal 3 [r SCARD myset]
            assert_equal 1 [r EXISTS myset]

            assert_equal 1 [get_keys_with_volatile_items r]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "SPOP with a count at or above the physical size returns the live members and keeps the hidden ones - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            r SADD myset live1 live2 live3 expired
            make_members_expired r myset {expired}
            assert_equal {live1 live2 live3} [lsort [r SPOP myset 1000000000]]
            assert_equal 1 [r SCARD myset]
            assert_equal {} [r SMEMBERS myset]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "SPOP with a count untracks the last volatile member - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            r SADD myset plain1 plain2 volatile
            assert_equal {1} [r SEXPIRE myset $::live_member_ttl MEMBERS 1 volatile]
            for {set i 0} {$i < 100 && [r SISMEMBER myset volatile]} {incr i} {
                set popped [lindex [r SPOP myset 1] 0]
                if {$popped ne "volatile"} { r SADD myset $popped }
            }
            assert_equal 0 [r SISMEMBER myset volatile]
            assert_equal 0 [get_keys_with_volatile_items r]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "SPOP without a count never returns an expired member - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            create_numbered_set myset 10
            make_members_expired r myset {m0 m1 m2 m3}
            for {set i 0} {$i < 6} {incr i} {
                set m [r SPOP myset]
                assert {$m ne {}}
                assert_none_of [list $m] {m0 m1 m2 m3}
            }
            assert_equal {} [r SPOP myset]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "SPOP with a count keeps the TTLs of the members that remain - $encoding" {
            r FLUSHALL
            use_set_encoding $encoding
            create_numbered_set myset 10
            assert_equal {1 1 1 1 1 1 1 1 1 1} \
                [r SEXPIRE myset $::live_member_ttl MEMBERS 10 m0 m1 m2 m3 m4 m5 m6 m7 m8 m9]
            set popped [r SPOP myset 9]
            assert_equal 9 [llength $popped]
            assert_equal 1 [r SCARD myset]
            assert_range [set_member_ttl r myset [lindex [r SMEMBERS myset] 0]] 1 $::live_member_ttl
        }

        test "SRANDMEMBER without a count is a read - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            create_numbered_set myset 10
            make_members_expired r myset {m0 m1 m2 m3}
            set expired_before [s expired_set_members]

            set got {}
            set signals [capture_mutation_signals myset {
                for {set i 0} {$i < 20} {incr i} { lappend got [r SRANDMEMBER myset] }
            }]
            assert_equal 20 [llength $got]
            assert_none_of $got {m0 m1 m2 m3}
            assert_equal {{} 0 0} $signals
            assert_equal 10 [r SCARD myset]
            assert_equal $expired_before [s expired_set_members]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "SRANDMEMBER with a positive count is a read - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            create_numbered_set myset 10
            make_members_expired r myset {m0 m1 m2 m3}
            set expired_before [s expired_set_members]

            set signals [capture_mutation_signals myset {
                # Above the live size every live member comes back exactly once.
                set above [r SRANDMEMBER myset 20]
                # Below it, still without duplicates.
                set below [r SRANDMEMBER myset 4]
                set single [r SRANDMEMBER myset 1]
            }]
            assert_equal {m4 m5 m6 m7 m8 m9} [lsort $above]
            assert_equal 4 [llength $below]
            assert_equal 4 [llength [lsort -unique $below]]
            assert_none_of $below {m0 m1 m2 m3}
            assert_equal 1 [llength $single]
            assert_none_of $single {m0 m1 m2 m3}
            assert_equal {{} 0 0} $signals
            assert_equal 10 [r SCARD myset]
            assert_equal $expired_before [s expired_set_members]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "SRANDMEMBER with a negative count is a read - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            create_numbered_set myset 10
            make_members_expired r myset {m0 m1 m2 m3}
            set expired_before [s expired_set_members]

            set signals [capture_mutation_signals myset {
                # A negative count returns exactly the requested number of
                # elements, repeats allowed, drawn only from the live members.
                set got [r SRANDMEMBER myset -50]
            }]
            assert_equal 50 [llength $got]
            assert_none_of $got {m0 m1 m2 m3}
            assert_equal {{} 0 0} $signals
            assert_equal 10 [r SCARD myset]
            assert_equal $expired_before [s expired_set_members]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "SRANDMEMBER finds the one live member of a mostly-expired set - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            create_numbered_set myset 10
            make_members_expired r myset {m0 m1 m2 m3 m4 m5 m6 m7 m8}
            for {set i 0} {$i < 20} {incr i} {
                assert_equal m9 [r SRANDMEMBER myset]
            }
            assert_equal 10 [r SCARD myset]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "SRANDMEMBER on a set of only expired members returns nothing - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            r SADD myset a b c
            make_members_expired r myset {a b c}
            assert_equal {{} 0 0} [capture_mutation_signals myset {
                assert_equal {} [r SRANDMEMBER myset]
                assert_equal {} [r SRANDMEMBER myset 20]
                assert_equal {} [r SRANDMEMBER myset 2]
                assert_equal {} [r SRANDMEMBER myset -20]
            }]
            assert_equal 3 [r SCARD myset]
            assert_equal 1 [r EXISTS myset]

            assert_equal 1 [get_keys_with_volatile_items r]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "SSCAN skips expired members - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            r SADD myset a b c d
            make_members_expired r myset {a b}
            # SSCAN may duplicate and reorder members.
            set cursor 0
            set found {}
            while 1 {
                set res [r SSCAN myset $cursor]
                set cursor [lindex $res 0]
                foreach m [lindex $res 1] { lappend found $m }
                if {$cursor == 0} break
            }
            assert_equal {c d} [lsort -unique $found]
            assert_equal 4 [r SCARD myset]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "SREM ignores expired members - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            r SADD myset a b c
            make_members_expired r myset {b}
            assert_equal 1 [r SREM myset a b]
            assert_equal {c} [r SMEMBERS myset]
            assert_equal 2 [r SCARD myset]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "SREM of the last live member leaves the key to active expiry - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            r SADD myset a b c
            make_members_expired r myset {b c}
            assert_equal 1 [r SREM myset a]
            assert_equal 2 [r SCARD myset]
            assert_equal {} [r SMEMBERS myset]

            assert_equal 1 [get_keys_with_volatile_items r]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "Set membership and TTL reads hide expired members - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            r SADD myset live1 gone1 live2 gone2
            make_members_expired r myset {gone1 gone2}
            assert_equal {1 0 1 0 0} [r SMISMEMBER myset live1 gone1 live2 gone2 missing]
            assert_equal {-2 -2} [r STTL myset MEMBERS 2 gone1 missing]
            assert_equal 4 [r SCARD myset]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}
        use_set_encoding listpack
    }

    test "SRANDMEMBER with a negative count samples more than 1200 distinct members" {
        flush_and_disable_active_expiry
        set members {}
        for {set i 0} {$i < 1500} {incr i} { lappend members m$i }
        r SADD myset {*}$members expired
        make_members_expired r myset {expired}

        set got [r SRANDMEMBER myset -20000]
        assert_equal 20000 [llength $got]
        assert_none_of $got {expired}
        assert {[llength [lsort -unique $got]] > 1200}
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test "SINTERSTORE with the destination equal to a volatile source" {
        flush_and_disable_active_expiry
        r SADD k{t} a b c
        make_members_expired r k{t} {c}
        assert_equal {1} [r SEXPIRE k{t} $::live_member_ttl MEMBERS 1 a]
        assert_equal 2 [r SINTERSTORE k{t} k{t}]
        assert_equal {a b} [lsort [r SMEMBERS k{t}]]
        assert_equal {-1 -1} [r STTL k{t} MEMBERS 2 a b]
        assert_equal 0 [get_keys_with_volatile_items r]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

# Removing the last volatile member flips the object to non-volatile before the
# key is deleted, so the deletion must still untrack it.
    foreach {what members empty} {
        SREM                        {a}   {assert_equal 1 [r SREM myset a]}
        SPOP                        {a}   {assert_equal a [r SPOP myset]}
        {SPOP with a count}         {a b} {assert_equal {a b} [lsort [r SPOP myset 2]]}
        {an expiration in the past} {a}   {assert_equal {2} [r SPEXPIREAT myset 1 MEMBERS 1 a]}
    } {
        test "$what empties a volatile set, deleting and untracking the key" {
            r FLUSHALL
            assert_equal 1 [r SADDEX myset PX 60000 MEMBERS [llength $members] {*}$members]
            assert_equal 1 [get_keys_with_volatile_items r]
            eval $empty
            assert_equal 0 [r EXISTS myset]
            assert_equal 0 [get_keys_with_volatile_items r]
        }
    }

    test "SMOVE of the only volatile member deletes the source and untracks it" {
        r FLUSHALL
        assert_equal 1 [r SADDEX src{t} PX 60000 MEMBERS 1 a]
        assert_equal 1 [get_keys_with_volatile_items r]
        assert_equal 1 [r SMOVE src{t} dst{t} a]
        assert_equal 0 [r EXISTS src{t}]
        assert_equal 1 [get_keys_with_volatile_items r]
        r DEL dst{t}
        assert_equal 0 [get_keys_with_volatile_items r]
    }

    # SORT sizes its vector from SCARD, which counts expired members it must skip.
    test "SORT skips an expired member" {
        flush_and_disable_active_expiry
        r SADD myset 1 2 3 4 5
        make_members_expired r myset {3}
        assert_equal 5 [r SCARD myset]

        assert_equal {1 2 4 5} [r SORT myset]
        assert_equal {4 5} [r SORT myset LIMIT 2 10]
        assert_equal 5 [r SCARD myset]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {MEMORY USAGE of a volatile set exceeds a plain set with the same members} {
        r FLUSHALL
        use_set_encoding hashtable

        set members {}
        for {set i 0} {$i < 64} {incr i} { lappend members m$i }
        r SADD plainset {*}$members
        r SADD volatile {*}$members

        set plain_mem [r MEMORY USAGE plainset]
        r SEXPIRE volatile 100000 MEMBERS 64 {*}$members
        set volatile_mem [r MEMORY USAGE volatile]

        assert_morethan $volatile_mem $plain_mem

        r SPERSIST volatile MEMBERS 64 {*}$members
        assert_equal $plain_mem [r MEMORY USAGE volatile]

        use_set_encoding listpack
    } {OK}

    test {Active expiry keeps working after the key is deleted and re-created} {
        r FLUSHALL
        r SADDEX myset PX 50 MEMBERS 1 m1
        wait_for_condition 100 100 {
            [r EXISTS myset] == 0
        } else {
            fail "first generation of the key was not reclaimed"
        }

        set initial_expired [s expired_set_members]
        r SADD myset survivor
        r SADDEX myset PX 50 MEMBERS 1 doomed
        wait_for_set_active_expiry r myset 1 $initial_expired 1
        assert_equal {survivor} [r SMEMBERS myset]
    }

    test {DEL of a volatile set untracks the key} {
        r FLUSHALL
        r SADDEX myset PX 60000 MEMBERS 1 a
        assert_equal 1 [get_keys_with_volatile_items r]
        r DEL myset
        assert_equal 0 [get_keys_with_volatile_items r]
    }
}

# Replicas and AOF loading retain expired members, so rewrites name only members changed by the primary.
start_server {tags {"setexpire external:skip"}} {
    test {SEXPIRE, SPEXPIRE, SEXPIREAT and SPEXPIREAT propagate as SPEXPIREAT} {
        r FLUSHALL
        set at_sec [expr {[clock seconds] + 100}]
        set at_ms [expr {[clock milliseconds] + 100000}]
        r SADD myset m1 m2 m3 m4
        set repl [attach_to_replication_stream]

        r SEXPIRE myset 100 MEMBERS 1 m1
        r SPEXPIRE myset 100000 MEMBERS 1 m2
        r SEXPIREAT myset $at_sec MEMBERS 1 m3
        r SPEXPIREAT myset $at_ms MEMBERS 1 m4
        set m1_at [set_member_pexpiretime r myset m1]
        set m2_at [set_member_pexpiretime r myset m2]

        assert_replication_stream $repl [subst {
            {select *}
            {spexpireat myset $m1_at MEMBERS 1 m1}
            {spexpireat myset $m2_at MEMBERS 1 m2}
            {spexpireat myset [expr {$at_sec * 1000}] MEMBERS 1 m3}
            {spexpireat myset $at_ms MEMBERS 1 m4}
        }]
        close_replication_stream $repl
    }

    test {SADDEX EX, PX and EXAT are propagated as PXAT} {
        r FLUSHALL
        set at_sec [expr {[clock seconds] + 100}]
        set repl [attach_to_replication_stream]

        r SADDEX myset EX 100 MEMBERS 1 m1
        r SADDEX myset PX 100000 MEMBERS 1 m2
        r SADDEX myset EXAT $at_sec MEMBERS 1 m3
        set m1_at [set_member_pexpiretime r myset m1]
        set m2_at [set_member_pexpiretime r myset m2]

        assert_replication_stream $repl [subst {
            {select *}
            {saddex myset PXAT $m1_at MEMBERS 1 m1}
            {saddex myset PXAT $m2_at MEMBERS 1 m2}
            {saddex myset PXAT [expr {$at_sec * 1000}] MEMBERS 1 m3}
        }]
        close_replication_stream $repl
    }

    test {SADDEX does not propagate condition arguments} {
        r FLUSHALL
        set at_ms [expr {[clock milliseconds] + 100000}]
        r SADD myset m1
        set repl [attach_to_replication_stream]

        r SADDEX myset XX MXX PXAT $at_ms MEMBERS 1 m1
        r SADDEX myset MNX PXAT $at_ms MEMBERS 1 m2
        r SADDEX other NX PXAT $at_ms MEMBERS 1 m1

        assert_replication_stream $repl [subst {
            {select *}
            {saddex myset PXAT $at_ms MEMBERS 1 m1}
            {saddex myset PXAT $at_ms MEMBERS 1 m2}
            {saddex other PXAT $at_ms MEMBERS 1 m1}
        }]
        close_replication_stream $repl
    }

    test {SADDEX KEEPTTL is propagated as KEEPTTL} {
        r FLUSHALL
        r SADDEX myset EX 100 MEMBERS 1 m1
        set repl [attach_to_replication_stream]

        # Adding m2 makes the otherwise no-op KEEPTTL command propagate.
        r SADDEX myset KEEPTTL MEMBERS 2 m1 m2
        r SADDEX myset XX KEEPTTL MEMBERS 2 m1 m3

        assert_replication_stream $repl {
            {select *}
            {saddex myset KEEPTTL MEMBERS 2 m1 m2}
            {saddex myset KEEPTTL MEMBERS 2 m1 m3}
        }
        close_replication_stream $repl
    }

    test {SEXPIRE with a time in the past is propagated as SREM} {
        r FLUSHALL
        r SADD myset m1 m2
        set repl [attach_to_replication_stream]

        r SEXPIRE myset 0 MEMBERS 2 m1 nomember
        r SEXPIREAT myset 1 MEMBERS 1 m2
        r SADD marker x

        # Replaying SREM deletes the empty set, so no separate key deletion is needed.
        assert_replication_stream $repl {
            {select *}
            {srem myset m1}
            {srem myset m2}
            {sadd marker x}
        }
        close_replication_stream $repl
    }

    test {SADDEX with a time in the past is propagated as SREM} {
        r FLUSHALL
        r SADD myset m1 m2
        set repl [attach_to_replication_stream]

        r SADDEX myset EX 0 MEMBERS 2 m1 nomember
        r SADDEX myset PXAT 1 MEMBERS 1 m2

        assert_replication_stream $repl {
            {select *}
            {srem myset m1}
            {srem myset m2}
        }
        close_replication_stream $repl
    }

    test {SADDEX MXX with a past expiration removes volatile members} {
        r FLUSHALL
        use_set_encoding hashtable
        r SADDEX myset EX $::live_member_ttl MEMBERS 2 m1 m2
        set repl [attach_to_replication_stream]

        assert_equal 1 [r SADDEX myset MXX EX 0 MEMBERS 2 m1 m2]

        assert_replication_stream $repl {
            {select *}
            {srem myset m1 m2}
        }
        assert_equal {} [read_from_replication_stream $repl]
        close_replication_stream $repl
        assert_equal 0 [r EXISTS myset]
        assert_equal 0 [get_keys_with_volatile_items r]
    }

    test {SADDEX MXX with a past expiration removes duplicate volatile members once} {
        r FLUSHALL
        use_set_encoding hashtable
        r SADDEX myset EX $::live_member_ttl MEMBERS 1 m1
        set repl [attach_to_replication_stream]

        assert_equal 1 [r SADDEX myset MXX EX 0 MEMBERS 2 m1 m1]

        # The second argument names a member the first one already removed.
        assert_replication_stream $repl {
            {select *}
            {srem myset m1}
        }
        assert_equal {} [read_from_replication_stream $repl]
        close_replication_stream $repl
        assert_equal 0 [r EXISTS myset]
        assert_equal 0 [get_keys_with_volatile_items r]
        use_set_encoding listpack
    } {OK}

    test {SREM with expired and live arguments propagates only removed members} {
        flush_and_disable_active_expiry
        r SADD myset live expired keep
        make_members_expired r myset {expired}
        set repl [attach_to_replication_stream]

        assert_equal 1 [r SREM myset live expired missing]
        assert_equal 1 [r SREM myset expired keep missing]
        assert_replication_stream $repl {
            {select *}
            {srem myset live}
            {srem myset keep}
        }
        close_replication_stream $repl
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {SADD over an expired member propagates SREM before SADD} {
        flush_and_disable_active_expiry
        r SADD myset m1 keepme
        make_members_expired r myset {m1}

        set repl [attach_to_replication_stream]
        r SADD myset m1

        # alsoPropagate() opens MULTI before SELECT.
        assert_replication_stream $repl {
            {multi}
            {select *}
            {srem myset m1}
            {sadd myset m1}
            {exec}
        }
        close_replication_stream $repl
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {SADDEX KEEPTTL over an expired member propagates SREM first} {
        flush_and_disable_active_expiry
        r SADD myset m1 keepme
        make_members_expired r myset {m1}

        set repl [attach_to_replication_stream]
        r SADDEX myset KEEPTTL MEMBERS 1 m1

        assert_replication_stream $repl {
            {multi}
            {select *}
            {srem myset m1}
            {saddex myset KEEPTTL MEMBERS 1 m1}
            {exec}
        }
        close_replication_stream $repl
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {SMOVE onto an expired destination member propagates SREM first} {
        flush_and_disable_active_expiry
        r SADD src{t} m1
        r SADD dst{t} m1 other
        make_members_expired r dst{t} {m1}

        set repl [attach_to_replication_stream]
        r SMOVE src{t} dst{t} m1

        assert_replication_stream $repl {
            {multi}
            {select *}
            {srem dst{t} m1}
            {smove src{t} dst{t} m1}
            {exec}
        }
        close_replication_stream $repl
        assert_equal 2 [r SCARD dst{t}]
        assert_equal {m1 other} [lsort [r SMEMBERS dst{t}]]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {SADD replacement preserves the containing key TTL during replay} {
        flush_and_disable_active_expiry
        r SADD myset m1
        set key_at [expr {[clock milliseconds] + 60000}]
        r PEXPIREAT myset $key_at
        make_members_expired r myset {m1}
        set repl [attach_to_replication_stream]

        assert_equal 1 [r SADD myset m1]
        assert_replication_stream $repl [subst {
            {multi}
            {select *}
            {srem myset m1}
            {sadd myset m1}
            {pexpireat myset $key_at}
            {exec}
        }]
        close_replication_stream $repl
        assert_equal $key_at [r PEXPIRETIME myset]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {SADDEX KEEPTTL replacement preserves the containing key TTL during replay} {
        flush_and_disable_active_expiry
        r SADD myset m1
        set key_at [expr {[clock milliseconds] + 60000}]
        r PEXPIREAT myset $key_at
        make_members_expired r myset {m1}
        set repl [attach_to_replication_stream]

        assert_equal 1 [r SADDEX myset KEEPTTL MEMBERS 1 m1]
        assert_replication_stream $repl [subst {
            {multi}
            {select *}
            {srem myset m1}
            {saddex myset KEEPTTL MEMBERS 1 m1}
            {pexpireat myset $key_at}
            {exec}
        }]
        close_replication_stream $repl
        assert_equal $key_at [r PEXPIRETIME myset]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {SMOVE replacement preserves the destination key TTL during replay} {
        flush_and_disable_active_expiry
        r SADD src{t} m1
        r SADD dst{t} m1
        set key_at [expr {[clock milliseconds] + 60000}]
        r PEXPIREAT dst{t} $key_at
        make_members_expired r dst{t} {m1}
        set repl [attach_to_replication_stream]

        assert_equal 1 [r SMOVE src{t} dst{t} m1]
        assert_replication_stream $repl [subst {
            {multi}
            {select *}
            {srem dst{t} m1}
            {smove src{t} dst{t} m1}
            {pexpireat dst{t} $key_at}
            {exec}
        }]
        close_replication_stream $repl
        assert_equal $key_at [r PEXPIRETIME dst{t}]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {Active expiry propagates SREM and no DEL while members remain} {
        r FLUSHALL
        r SADD myset keepme
        set repl [attach_to_replication_stream]

        set at_ms [expr {[clock milliseconds] + 500}]
        r SADDEX myset PXAT $at_ms MEMBERS 2 m1 m2
        wait_for_condition 100 100 {
            [r SCARD myset] == 1
        } else {
            fail "active expiry did not reclaim m1/m2"
        }
        r SADD marker x

        assert_replication_stream $repl [subst {
            {select *}
            {saddex myset PXAT $at_ms MEMBERS 2 m1 m2}
            {srem myset m1 m2}
            {sadd marker x}
        }]
        close_replication_stream $repl
    }

    test {Active expiry of the last member propagates SREM then UNLINK} {
        r FLUSHALL
        set repl [attach_to_replication_stream]

        set at_ms [expr {[clock milliseconds] + 500}]
        r SADDEX myset PXAT $at_ms MEMBERS 1 m1
        wait_for_condition 100 100 {
            [r EXISTS myset] == 0
        } else {
            fail "active expiry did not delete the emptied key"
        }

        # lazyfree-lazy-expire (default yes) makes the key deletion an UNLINK.
        assert_replication_stream $repl [subst {
            {select *}
            {saddex myset PXAT $at_ms MEMBERS 1 m1}
            {multi}
            {srem myset m1}
            {unlink myset}
            {exec}
        }]
        close_replication_stream $repl
    }

    test {SRANDMEMBER propagates nothing} {
        flush_and_disable_active_expiry
        r SADD myset live1 live2 e1 e2
        make_members_expired r myset {e1 e2}

        set repl [attach_to_replication_stream]
        r SRANDMEMBER myset
        r SRANDMEMBER myset 2
        r SRANDMEMBER myset 20
        r SRANDMEMBER myset -20
        r SADD marker x

        assert_replication_stream $repl {
            {select *}
            {sadd marker x}
        }
        close_replication_stream $repl
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {SPOP without a count propagates only the member it returned} {
        flush_and_disable_active_expiry
        r SADD myset live1 live2 e1 e2
        make_members_expired r myset {e1 e2}

        set repl [attach_to_replication_stream]
        set popped [r SPOP myset]
        r SADD marker x

        assert_replication_stream $repl [subst {
            {select *}
            {srem myset $popped}
            {sadd marker x}
        }]
        close_replication_stream $repl
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {SPOP with a count propagates only the live members it returned} {
        flush_and_disable_active_expiry
        r SADD myset live1 live2 live3 e1 e2
        make_members_expired r myset {e1 e2}
        set repl [attach_to_replication_stream]
        set popped [r SPOP myset 2]
        r SADD marker x
        assert_replication_stream $repl [subst {
            {select *}
            {srem myset [join $popped]}
            {sadd marker x}
        }]
        close_replication_stream $repl
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    # Every replaced expired member's SREM precedes the single SADD, however
    # many members one SADD replaces.
    test {SADD over 1025 expired members propagates the SREM of every one before the SADD} {
        flush_and_disable_active_expiry
        create_numbered_set myset 1025
        set members [r SMEMBERS myset]
        make_members_expired r myset $members

        set repl [attach_to_replication_stream]
        assert_equal 1025 [r SADD myset {*}$members]
        assert_equal {multi} [read_from_replication_stream $repl]
        assert_match {select *} [read_from_replication_stream $repl]
        assert_equal [list srem myset {*}[lrange $members 0 1023]] [read_from_replication_stream $repl]
        assert_equal [list srem myset [lindex $members 1024]] [read_from_replication_stream $repl]
        assert_equal [list sadd myset {*}$members] [read_from_replication_stream $repl]
        assert_equal {exec} [read_from_replication_stream $repl]
        close_replication_stream $repl
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    # A STORE result depends on the live view, so the result is propagated, not the command.
    foreach {cmd expected} {SUNIONSTORE {a b c} SINTERSTORE {a} SDIFFSTORE {c}} {
        test "$cmd with an expired member in a source propagates UNLINK plus SADD" {
            flush_and_disable_active_expiry
            r SADD s1{t} a expired c
            r SADD s2{t} a b
            r SADD dst{t} stale
            make_members_expired r s1{t} {expired}

            set repl [attach_to_replication_stream]
            assert_equal [llength $expected] [r $cmd dst{t} s1{t} s2{t}]

            assert_equal {multi} [read_from_replication_stream $repl]
            assert_match {select *} [read_from_replication_stream $repl]
            assert_equal [list unlink dst{t}] [read_from_replication_stream $repl]
            # Member order is the destination's iteration order.
            set line [read_from_replication_stream $repl]
            assert_equal sadd [lindex $line 0]
            assert_equal dst{t} [lindex $line 1]
            assert_equal $expected [lsort [lrange $line 2 end]]
            assert_equal {exec} [read_from_replication_stream $repl]
            assert_equal {} [read_from_replication_stream $repl]
            close_replication_stream $repl
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}
    }

    foreach cmd {SINTERSTORE SDIFFSTORE} {
        test "$cmd with a volatile source and an empty result propagates only UNLINK" {
            flush_and_disable_active_expiry
            r SADD s1{t} expired
            r SADD s2{t} b
            make_members_expired r s1{t} {expired}

            set repl [attach_to_replication_stream]
            # A replica sees the hidden member as live, so an absent destination must
            # receive the deletion rather than the command it would recompute.
            assert_equal 0 [r $cmd dst{t} s1{t} s2{t}]
            r SADD dst{t} stale
            assert_equal 0 [r $cmd dst{t} s1{t} s2{t}]

            assert_replication_stream $repl {
                {select *}
                {unlink dst{t}}
                {sadd dst{t} stale}
                {unlink dst{t}}
            }
            close_replication_stream $repl
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}
    }

    # propagateStoreAsEffects flushes the stored items in batches of 1024.
    test {A STORE of 1025 items propagates them in two batches} {
        flush_and_disable_active_expiry
        create_numbered_set s1{t} 1025
        r SADD s1{t} expired
        make_members_expired r s1{t} {expired}
        r SADD dst{t} stale
        r RPUSH l{t} stale

        set repl [attach_to_replication_stream]
        assert_equal 1025 [r SUNIONSTORE dst{t} s1{t}]
        assert_equal 1025 [r SORT s1{t} ALPHA STORE l{t}]

        assert_equal {multi} [read_from_replication_stream $repl]
        assert_match {select *} [read_from_replication_stream $repl]
        assert_equal [list unlink dst{t}] [read_from_replication_stream $repl]
        set line [read_from_replication_stream $repl]
        assert_equal {sadd dst{t}} [lrange $line 0 1]
        assert_equal 1024 [llength [lrange $line 2 end]]
        set stored [lrange $line 2 end]
        set line [read_from_replication_stream $repl]
        assert_equal {sadd dst{t}} [lrange $line 0 1]
        assert_equal 1 [llength [lrange $line 2 end]]
        set stored [concat $stored [lrange $line 2 end]]
        assert_equal {exec} [read_from_replication_stream $repl]
        assert_equal [lsort [r SMEMBERS dst{t}]] [lsort $stored]

        assert_equal {multi} [read_from_replication_stream $repl]
        assert_equal [list unlink l{t}] [read_from_replication_stream $repl]
        set line [read_from_replication_stream $repl]
        assert_equal {rpush l{t}} [lrange $line 0 1]
        assert_equal 1024 [llength [lrange $line 2 end]]
        set pushed [lrange $line 2 end]
        set line [read_from_replication_stream $repl]
        assert_equal {rpush l{t}} [lrange $line 0 1]
        assert_equal 1 [llength [lrange $line 2 end]]
        set pushed [concat $pushed [lrange $line 2 end]]
        assert_equal {exec} [read_from_replication_stream $repl]
        assert_equal [r LRANGE l{t} 0 -1] $pushed
        close_replication_stream $repl
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {A STORE with no volatile source still propagates the command verbatim} {
        r FLUSHALL
        r SADD s1{t} a b c
        r SADD s2{t} b c d
        set repl [attach_to_replication_stream]

        r SUNIONSTORE u{t} s1{t} s2{t}
        r SINTERSTORE i{t} s1{t} s2{t}
        r SDIFFSTORE d{t} s1{t} s2{t}

        assert_replication_stream $repl {
            {select *}
            {sunionstore u{t} s1{t} s2{t}}
            {sinterstore i{t} s1{t} s2{t}}
            {sdiffstore d{t} s1{t} s2{t}}
        }
        assert_equal {} [read_from_replication_stream $repl]
        close_replication_stream $repl
    }

    test {A STORE over a volatile source with nothing hidden propagates the command verbatim} {
        r FLUSHALL
        # 200 volatile members make the expiry index a RAX, whose bucket key is the
        # deadline rounded up to an 8192 ms window; entering the window early keeps
        # that key ahead of the clock while every member is still live.
        while {[clock milliseconds] % 8192 >= 1024} { after 50 }
        set members {}
        for {set i 0} {$i < 200} {incr i} { lappend members live$i }
        set deadline [expr {[clock milliseconds] + 6000}]
        r SADDEX s1{t} PXAT $deadline MEMBERS 200 {*}$members
        r SADD s2{t} live0 live1
        set repl [attach_to_replication_stream]

        assert_equal 2 [r SINTERSTORE dst{t} s1{t} s2{t}]
        assert_equal 200 [r SUNIONSTORE u{t} s1{t} s2{t}]
        assert_equal 198 [r SDIFFSTORE d{t} s1{t} s2{t}]
        assert_equal 200 [r SORT s1{t} ALPHA STORE l{t}]

        assert_replication_stream $repl {
            {select *}
            {sinterstore dst{t} s1{t} s2{t}}
            {sunionstore u{t} s1{t} s2{t}}
            {sdiffstore d{t} s1{t} s2{t}}
            {sort s1{t} ALPHA STORE l{t}}
        }
        close_replication_stream $repl
        assert {[clock milliseconds] < $deadline}
    }
    test {SORT STORE with a volatile source propagates UNLINK plus RPUSH} {
        flush_and_disable_active_expiry
        r SADD src{t} 1 2 3 4 5
        r RPUSH dst{t} stale
        make_members_expired r src{t} {3}

        set repl [attach_to_replication_stream]
        assert_equal 4 [r SORT src{t} STORE dst{t}]

        assert_replication_stream $repl {
            {multi}
            {select *}
            {unlink dst{t}}
            {rpush dst{t} 1 2 4 5}
            {exec}
        }
        assert_equal {} [read_from_replication_stream $repl]
        close_replication_stream $repl
        assert_equal 5 [r SCARD src{t}]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {SORT STORE over only expired members propagates the destination's UNLINK even when it is absent} {
        flush_and_disable_active_expiry
        r SADD src{t} 1 2 3
        make_members_expired r src{t} {1 2 3}

        set repl [attach_to_replication_stream]
        assert_equal 0 [r SORT src{t} STORE dst{t}]
        r RPUSH dst{t} stale
        assert_equal 0 [r SORT src{t} STORE dst{t}]

        assert_replication_stream $repl {
            {select *}
            {unlink dst{t}}
            {rpush dst{t} stale}
            {unlink dst{t}}
        }
        close_replication_stream $repl
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {SPERSIST naming an expired member propagates only the members persisted} {
        flush_and_disable_active_expiry
        seed_expired_member r myset

        set repl [attach_to_replication_stream]
        assert_equal {1 -2} [r SPERSIST myset MEMBERS 2 live expired]

        assert_replication_stream $repl {
            {select *}
            {spersist myset MEMBERS 1 live}
        }
        assert_equal {} [read_from_replication_stream $repl]
        close_replication_stream $repl
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {SEXPIRE naming an expired member propagates only the members updated} {
        flush_and_disable_active_expiry
        seed_expired_member r myset

        set repl [attach_to_replication_stream]
        assert_equal {1 -2} [r SEXPIRE myset 300 MEMBERS 2 live expired]
        set at [set_member_pexpiretime r myset live]

        assert_replication_stream $repl [subst {
            {select *}
            {spexpireat myset $at MEMBERS 1 live}
        }]
        assert_equal {} [read_from_replication_stream $repl]
        close_replication_stream $repl
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {SPEXPIREAT and SPERSIST keep the members they leave unchanged when skipping an expired member} {
        flush_and_disable_active_expiry
        seed_expired_member r myset
        r SADD myset nottl
        set at [expr {[clock milliseconds] + 1000000}]

        set repl [attach_to_replication_stream]
        assert_equal {0 -2 -2 1} [r SPEXPIREAT myset $at XX GT MEMBERS 4 nottl expired missing live]
        assert_equal {-1 -2 1} [r SPERSIST myset MEMBERS 3 nottl expired live]

        assert_replication_stream $repl [subst {
            {select *}
            {spexpireat myset $at XX GT MEMBERS 2 nottl live}
            {spersist myset MEMBERS 2 nottl live}
        }]
        close_replication_stream $repl
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    foreach encoding {listpack hashtable} {
        test "DEBUG RELOAD keeps member TTLs - $encoding" {
            r FLUSHALL
            use_set_encoding $encoding

            r SADD myset m1 m2 m3
            set exp [expr {[clock milliseconds] + 100000}]
            r SPEXPIREAT myset $exp MEMBERS 1 m1
            assert_encoding $encoding myset
            assert_equal 1 [get_keys_with_volatile_items r]

            r DEBUG RELOAD

            assert_encoding $encoding myset
            assert_equal 3 [r SCARD myset]
            assert_equal $exp [set_member_pexpiretime r myset m1]
            assert_equal -1 [set_member_ttl r myset m3]
            assert_equal 1 [get_keys_with_volatile_items r]

            use_set_encoding listpack
        } {OK} {needs:debug}
    }

    test {DEBUG RELOAD of a set whose members are all expired drops the key} {
        flush_and_disable_active_expiry

        r SADD myset m1 m2 m3
        make_members_expired r myset {m1 m2 m3}

        r DEBUG RELOAD

        assert_equal 0 [r EXISTS myset]
        assert_equal 0 [get_keys_with_volatile_items r]

        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {RESTORE of an all-expired payload loads the expired members} {
        flush_and_disable_active_expiry

        r SADD myset m1 m2 m3
        make_members_expired r myset {m1 m2 m3}
        set serialized [r DUMP myset]
        r DEL myset
        r RESTORE myset 0 $serialized

        assert_equal 3 [r SCARD myset]
        assert_equal {} [r SMEMBERS myset]

        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    # RESTORE keeps expired members, so both copies reach the duplicate check.
    foreach encoding {listpack hashtable} {
        test "RESTORE rejects a payload that repeats an expired member - $encoding" {
            r FLUSHALL
            use_set_encoding $encoding
            r DEBUG SET-SKIP-CHECKSUM-VALIDATION 1
            assert_error "*Bad data format*" {r RESTORE myset 0 [set2_payload {aa 1 aa -1}]}
            assert_equal 0 [r EXISTS myset]
            r DEBUG SET-SKIP-CHECKSUM-VALIDATION 0
            use_set_encoding listpack
        } {OK} {needs:debug}
    }

    test {COPY of a hashtable set keeps an expired member and its TTL} {
        flush_and_disable_active_expiry
        use_set_encoding hashtable
        r SADD src{t} expired l1 l2
        make_members_expired r src{t} {expired}
        assert_encoding hashtable src{t}

        r COPY src{t} dst{t}
        assert_equal 3 [r SCARD dst{t}]
        assert_equal 0 [r SISMEMBER dst{t} expired]

        use_set_encoding listpack
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    foreach cmd {COPY RENAME RESTORE} {
        test "$cmd keeps member TTLs" {
            r FLUSHALL

            r SADD myset{t} m1 m2 m3
            set exp [expr {[clock milliseconds] + 200000}]
            r SPEXPIREAT myset{t} $exp MEMBERS 1 m1

            switch $cmd {
                COPY { assert_equal 1 [r COPY myset{t} newset{t}] }
                RENAME { r RENAME myset{t} newset{t} }
                RESTORE { r RESTORE newset{t} 0 [r DUMP myset{t}] }
            }
            if {$cmd eq "RENAME"} {
                assert_equal 0 [r EXISTS myset{t}]
                assert_equal 1 [get_keys_with_volatile_items r]
            } else {
                assert_equal $exp [set_member_pexpiretime r myset{t} m1]
                assert_equal 2 [get_keys_with_volatile_items r]
            }

            assert_equal 3 [r SCARD newset{t}]
            assert_equal $exp [set_member_pexpiretime r newset{t} m1]
            assert_equal -1 [set_member_ttl r newset{t} m3]
        } {}
    }

    set RDB_TYPE_SET 2
    set RDB_TYPE_SET_LISTPACK 20
    # Type 23 is upstream's RDB_TYPE_PATH_HASH, so volatile sets take the next one.
    set RDB_TYPE_SET_2 24

    foreach {encoding rdb_type} [list listpack $RDB_TYPE_SET_LISTPACK hashtable $RDB_TYPE_SET] {
        test "DUMP of a $encoding set uses SET_2 only while a member has a TTL" {
            r FLUSHALL
            use_set_encoding $encoding
            r SADD myset a b c
            assert_encoding $encoding myset
            assert_equal $rdb_type [dump_payload_type [r DUMP myset]]

            assert_equal {1 1 1} [r SEXPIRE myset $::live_member_ttl MEMBERS 3 a b c]
            assert_equal $RDB_TYPE_SET_2 [dump_payload_type [r DUMP myset]]

            assert_equal {1 1 1} [r SPERSIST myset MEMBERS 3 a b c]
            assert_equal $rdb_type [dump_payload_type [r DUMP myset]]

            use_set_encoding listpack
        } {OK}
    }

    start_server {tags {needs:repl external:skip}} {
        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        set replica [srv 0 client]

        test {Full sync carries member TTLs} {
            $primary FLUSHALL

            set now [clock milliseconds]
            set m1_exp [expr {$now + 50000}]
            set m2_exp [expr {$now + 70000}]

            $primary SADD myset m1 m2 m3
            $primary SPEXPIREAT myset $m1_exp MEMBERS 1 m1
            $primary SPEXPIREAT myset $m2_exp MEMBERS 1 m2

            attach_replica $primary $replica $primary_host $primary_port

            assert_equal $m1_exp [set_member_pexpiretime $replica myset m1]
            assert_equal $m2_exp [set_member_pexpiretime $replica myset m2]
            assert_equal -1 [set_member_ttl $replica myset m3]
            assert_equal 3 [$replica SCARD myset]
            assert_equal 1 [get_keys_with_volatile_items $replica]
        }

        test {A replica never reclaims an expired member on its own} {
            $primary FLUSHALL
            $primary DEBUG SET-ACTIVE-EXPIRE 0

            $primary SADD myset m1 keepme
            make_members_expired $primary myset {m1}
            wait_for_ofs_sync $primary $replica

            assert_equal 2 [$replica SCARD myset]
            assert_equal {keepme} [$replica SMEMBERS myset]
            assert_equal 1 [get_keys_with_volatile_items $replica]

            # Only the primary's SREM removes it: the replica's own expiry
            # cycle has run while the primary reclaimed and counted nothing.
            set expired [status $primary expired_set_members]
            $primary DEBUG SET-ACTIVE-EXPIRE 1
            wait_for_set_active_expiry $primary myset 1 $expired 1
            wait_for_ofs_sync $primary $replica
            assert_equal 1 [$replica SCARD myset]
            assert_equal 0 [status $replica expired_set_members]
        } {} {needs:debug}

        test {A promoted replica reclaims its expired members itself} {
            $primary FLUSHALL
            $primary DEBUG SET-ACTIVE-EXPIRE 0

            $primary SADD myset m1 m2 keepme
            make_members_expired $primary myset {m1 m2}
            wait_for_ofs_sync $primary $replica

            assert_equal 3 [$replica SCARD myset]

            $replica replicaof no one
            wait_for_condition 100 100 {
                [status $replica role] eq "master"
            } else {
                fail "Replica didn't become master"
            }

            wait_for_condition 100 100 {
                [$replica SCARD myset] == 1
            } else {
                fail "promoted replica did not reclaim its expired members"
            }
            assert_equal {keepme} [$replica SMEMBERS myset]

            $primary DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test {SREM, SPERSIST and SPEXPIRE preserve member expiry state on replicas} {
            $primary FLUSHALL
            $primary DEBUG SET-ACTIVE-EXPIRE 0
            attach_replica $primary $replica $primary_host $primary_port

            foreach key {kr kp kx} {
                seed_expired_member $primary $key
            }

            assert_equal 1 [$primary SREM kr live]
            assert_equal {-2 1} [$primary SPERSIST kp MEMBERS 2 expired live]
            assert_equal {-2 1} [$primary SPEXPIRE kx 30000 XX MEMBERS 2 expired live]
            wait_for_ofs_sync $primary $replica

            foreach key {kr kp kx} {
                assert_equal [$primary SCARD $key] [$replica SCARD $key]
                assert_equal [$primary SPEXPIRETIME $key MEMBERS 2 expired live] \
                    [$replica SPEXPIRETIME $key MEMBERS 2 expired live]
            }

            $primary DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        # Startup RDB loading propagates skipped members; DEBUG RELOAD does not.
        test {A primary loading an RDB at startup feeds SREM for the expired members it skips} {
            $primary FLUSHALL
            $primary DEBUG SET-ACTIVE-EXPIRE 0
            $primary SADD myset live1 live2 expired
            make_members_expired $primary myset {expired}
            assert_equal 3 [$primary SCARD myset]
            wait_for_ofs_sync $primary $replica
            # The shutdown save records the final replication offset, so the
            # replica can partially resync and receive the fed SREM.
            $primary config set save "3600 1"

            restart_server -1 true false
            set primary [srv -1 client]
            wait_for_condition 50 100 {
                [status $replica master_link_status] eq {up} && [status $primary sync_partial_ok] == 1
            } else {
                fail "the replica did not partially resync with the restarted primary"
            }

            assert_equal 2 [$primary SCARD myset]
            assert_equal {live1 live2} [lsort [$primary SMEMBERS myset]]
            assert_equal 0 [get_keys_with_volatile_items $primary]
            wait_for_condition 50 100 {
                [$replica SCARD myset] == 2
            } else {
                fail "the replica never received SREM for the skipped member"
            }
            assert_equal {live1 live2} [lsort [$replica SMEMBERS myset]]
        } {} {needs:debug}
    }
}

tags {"aof external:skip"} {
    foreach rdb_preamble {"yes" "no"} {
        set defaults {appendonly {yes} appendfilename {appendonly.aof} appenddirname {appendonlydir} auto-aof-rewrite-percentage {0}}
        set server_path [tmpdir server.setexpire.aof]
        start_server_aof [list dir $server_path aof-use-rdb-preamble $rdb_preamble] {
            # start_server_aof runs this body in its own frame, so re-read the loop value.
            set rdb_preamble [lindex [r config get aof-use-rdb-preamble] 1]
            foreach encoding {listpack hashtable} {
                test "Member TTLs survive AOF rewrite and restart, preamble $rdb_preamble - $encoding" {
                    flush_and_disable_active_expiry
                    use_set_encoding $encoding
                    assert_equal 0 [get_keys_with_volatile_items r]

                    set long_expire [expr {[clock milliseconds] + 1000000000}]

                    for {set i 1} {$i <= 10} {incr i} {
                        r SADD myset p$i
                        r SADDEX myset PXAT $long_expire MEMBERS 1 m$i
                    }
                    set expired {}
                    for {set i 11} {$i <= 20} {incr i} { lappend expired m$i }
                    r SADD myset {*}$expired
                    make_members_expired r myset $expired
                    assert_equal 1 [get_keys_with_volatile_items r]

                    waitForBgrewriteaof r
                    r BGREWRITEAOF
                    waitForBgrewriteaof r
                    if {"$rdb_preamble" eq "no"} {
                        validate_aof_content [get_base_aof_path r] 20 0 SREM
                    }

                    restart_server 0 true false

                    assert_equal 20 [llength [r SMEMBERS myset]]
                    for {set i 1} {$i <= 10} {incr i} {
                        assert_equal $long_expire [set_member_pexpiretime r myset m$i]
                        assert_equal -1 [set_member_ttl r myset p$i]
                    }
                    assert_equal 1 [get_keys_with_volatile_items r]
                } {} {needs:debug}

                test "AOF rewrite after SPOP writes the remaining members, preamble $rdb_preamble - $encoding" {
                    flush_and_disable_active_expiry
                    use_set_encoding $encoding
                    set long_expire [expr {[clock milliseconds] + 1000000000}]
                    assert_equal 1 [r SADDEX myset PXAT $long_expire MEMBERS 3 a b c]
                    assert_equal 1 [llength [r SPOP myset 1]]
                    r SADD myset persistent
                    set before [lsort [r SMEMBERS myset]]

                    waitForBgrewriteaof r
                    r BGREWRITEAOF
                    waitForBgrewriteaof r
                    restart_server 0 true false

                    assert_equal $before [lsort [r SMEMBERS myset]]
                    assert_equal -1 [set_member_ttl r myset persistent]
                    assert_equal 1 [get_keys_with_volatile_items r]
                } {} {needs:debug}

                test "An expired-only AOF base preserves the key TTL for later writes, preamble $rdb_preamble - $encoding" {
                    flush_and_disable_active_expiry
                    use_set_encoding $encoding
                    r SADD myset expired
                    r PEXPIRE myset 600000
                    set key_expire [r PEXPIRETIME myset]
                    make_members_expired r myset {expired}

                    waitForBgrewriteaof r
                    r BGREWRITEAOF
                    waitForBgrewriteaof r
                    r SADD myset persistent
                    restart_server 0 true false

                    assert_equal {persistent} [r SMEMBERS myset]
                    assert_equal $key_expire [r PEXPIRETIME myset]
                } {} {needs:debug}
            }
        }
    }
}

start_cluster 2 0 {tags {external:skip cluster}} {
    set key myset

    test "MIGRATE carries member TTLs to the importing node" {
        R 0 FLUSHALL
        R 1 FLUSHALL

        set exp [expr {[clock milliseconds] + 300000}]
        R 0 SADD $key volatile1 volatile2 persistent
        R 0 SPEXPIREAT $key $exp MEMBERS 2 volatile1 volatile2
        assert_equal 1 [get_keys_with_volatile_items [Rn 0]]

        migrate_key_slot 0 1 $key

        assert_equal 3 [R 1 SCARD $key]
        assert_equal [list $exp $exp -1] \
            [R 1 SPEXPIRETIME $key MEMBERS 3 volatile1 volatile2 persistent]
        assert_equal 1 [get_keys_with_volatile_items [Rn 1]]
        assert_equal 0 [get_keys_with_volatile_items [Rn 0]]
        migrate_key_slot 1 0 $key
    }

    test "MIGRATE carries an expired member the exporting node had not reclaimed" {
        R 0 FLUSHALL
        R 1 FLUSHALL
        R 0 DEBUG SET-ACTIVE-EXPIRE 0
        R 1 DEBUG SET-ACTIVE-EXPIRE 0

        R 0 SADD $key live expired
        make_members_expired [Rn 0] $key {expired}
        assert_equal 2 [R 0 SCARD $key]

        migrate_key_slot 0 1 $key

        # MIGRATE preserves unreclaimed members for destination-side expiration.
        assert_equal 2 [R 1 SCARD $key]
        assert_equal {live} [R 1 SMEMBERS $key]

        R 1 DEBUG SET-ACTIVE-EXPIRE 1
        wait_for_condition 100 100 {
            [R 1 SCARD $key] == 1
        } else {
            fail "the importing node did not remove the expired member"
        }
        assert_equal 0 [get_keys_with_volatile_items [Rn 1]]

        R 0 DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}
}

# Inside one 8192 ms expiry-index window the index reports the earliest expiry as
# the window end while every hidden member is still present and unreclaimable.
start_server {tags {"setexpire needs:repl external:skip"} overrides {set-max-listpack-entries 0}} {
    start_server {} {
        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        set replica [srv 0 client]
        attach_replica $primary $replica $primary_host $primary_port

        test "SPOP with a count keeps hidden members while removing live ones" {
            $primary FLUSHALL
            $primary DEBUG SET-ACTIVE-EXPIRE 0
            $replica DEBUG SET-ACTIVE-EXPIRE 0
            set live_members [make_rax_window_set $primary myset 126 60]
            set physical [expr {126 + [llength $live_members]}]
            wait_for_ofs_sync $primary $replica
            assert_equal $physical [$replica SCARD myset]

            # Volatile sets remove live members in place so hidden members stay untouched.
            set popped [$primary SPOP myset 230]
            assert_inside_rax_window
            assert_equal [llength $live_members] [llength $popped]
            assert_equal [llength $popped] [llength [lsort -unique $popped]]
            assert_all_live $popped $live_members
            assert_equal {} [$primary SMEMBERS myset]

            # Only the returned members left the set, so the hidden ones are
            # still there, and the replica was told exactly the same.
            assert_equal [expr {$physical - [llength $popped]}] [$primary SCARD myset]
            wait_for_ofs_sync $primary $replica
            assert_equal [$primary SCARD myset] [$replica SCARD myset]
            assert_equal [lsort [$primary SMEMBERS myset]] [lsort [$replica SMEMBERS myset]]

            $primary DEBUG SET-ACTIVE-EXPIRE 1
            $replica DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        foreach cmd {SINTERSTORE SUNIONSTORE SDIFFSTORE} {
            test "$cmd over a source with hidden members stores the same set on the replica" {
                $primary FLUSHALL
                $primary DEBUG SET-ACTIVE-EXPIRE 0
                $replica DEBUG SET-ACTIVE-EXPIRE 0
                set live_members [make_rax_window_set $primary src{t} 126 60]
                set physical [expr {126 + [llength $live_members]}]
                # Live and hidden members of the source, so every set operation
                # has a result that depends on which of them are visible.
                $primary SADD other{t} live0 live1 live2 h0 h1
                wait_for_ofs_sync $primary $replica
                # The comparison only means something if the replica still holds
                # the members the primary is hiding.
                assert_equal $physical [$replica SCARD src{t}]

                switch $cmd {
                    SINTERSTORE { set expected [lrange $live_members 0 2] }
                    SUNIONSTORE { set expected [concat $live_members {h0 h1}] }
                    SDIFFSTORE { set expected [lrange $live_members 3 end] }
                }
                assert_equal [llength $expected] [$primary $cmd dst{t} src{t} other{t}]
                assert_inside_rax_window
                # A STORE reads its sources, so both sides must still hold them.
                assert_equal $physical [$primary SCARD src{t}]
                assert_equal [lsort $expected] [lsort [$primary SMEMBERS dst{t}]]
                wait_for_ofs_sync $primary $replica
                assert_equal $physical [$replica SCARD src{t}]
                assert_equal [lsort $expected] [lsort [$replica SMEMBERS dst{t}]]

                $primary DEBUG SET-ACTIVE-EXPIRE 1
                $replica DEBUG SET-ACTIVE-EXPIRE 1
            } {OK} {needs:debug}
        }

        test "SRANDMEMBER on a replica draws only live members and reclaims nothing" {
            $primary FLUSHALL
            $primary DEBUG SET-ACTIVE-EXPIRE 0
            $replica DEBUG SET-ACTIVE-EXPIRE 0
            set live_members [make_mostly_expired_set $primary myset 400 20 0]
            wait_for_ofs_sync $primary $replica

            # A replica read sees POLICY_KEEP_EXPIRED, so the members are hidden
            # there too and only the primary's SREM ever removes them.
            assert_equal 420 [$replica SCARD myset]
            set got [$replica SRANDMEMBER myset -200]
            assert_equal 200 [llength $got]
            assert_all_live $got $live_members
            assert_equal [lsort $live_members] [lsort [$replica SRANDMEMBER myset 20]]
            assert_equal [lsort $live_members] [lsort [$replica SMEMBERS myset]]
            assert_equal 420 [$replica SCARD myset]

            $primary DEBUG SET-ACTIVE-EXPIRE 1
            $replica DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "SPOP with a count equal to the size over live volatile members leaves no key" {
            $primary FLUSHALL
            $primary DEBUG SET-ACTIVE-EXPIRE 0
            # 200 volatile members make the expiry index a RAX, which reports the
            # start of the window holding its earliest deadline, so until that
            # deadline it claims an expired member the set does not have. Entering
            # the window early keeps the deadline inside it.
            while {[clock milliseconds] % 8192 >= 1024} { after 50 }
            set members {}
            for {set i 0} {$i < 200} {incr i} { lappend members live$i }
            set deadline [expr {[clock milliseconds] + 6000}]
            $primary SADDEX myset PXAT $deadline MEMBERS 200 {*}$members

            # A count at or above the size of a set the index calls partly
            # expired, of members it is all wrong about.
            set popped [$primary SPOP myset 200]
            assert {[clock milliseconds] < $deadline}
            assert_equal [lsort $members] [lsort $popped]

            # Every member left, so no key may remain on either side.
            assert_equal 0 [$primary EXISTS myset]
            assert_equal 0 [get_keys_with_volatile_items $primary]
            wait_for_ofs_sync $primary $replica
            assert_equal 0 [$replica EXISTS myset]

            $primary DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}
    }
}

start_server {tags {"setexpire external:skip"} overrides {set-max-listpack-entries 0}} {
    test "SRANDMEMBER with a count below the physical size returns unique live members" {
        r FLUSHALL
        r DEBUG SET-ACTIVE-EXPIRE 0
        set live_members [make_rax_window_set r myset 126 150]

        # Three times the count exceeds the physical size, which selects the
        # path that copies the set and subtracts down to the count.
        set got [r SRANDMEMBER myset 120]
        assert_inside_rax_window
        assert_equal 120 [llength $got]
        assert_equal 120 [llength [lsort -unique $got]]
        assert_all_live $got $live_members
        assert_equal [expr {126 + [llength $live_members]}] [r SCARD myset]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    # Inside the window the hidden members' bucket key is still ahead of the
    # clock, so it is the one bucket a census cannot settle from its key alone.
    test "SRANDMEMBER with a negative count draws only live members inside the window" {
        r FLUSHALL
        r DEBUG SET-ACTIVE-EXPIRE 0
        set live_members [make_rax_window_set r myset 126 150]

        set got [r SRANDMEMBER myset -600]
        assert_inside_rax_window
        assert_equal 600 [llength $got]
        assert_all_live $got $live_members
        assert_equal [expr {126 + [llength $live_members]}] [r SCARD myset]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    foreach {live_kind live_ttl live_plain} {volatile 20 0 persistent 0 20 mixed 10 10} {
        test "SRANDMEMBER draws only live members of a mostly-expired set - $live_kind live" {
            flush_and_disable_active_expiry
            set live_members [make_mostly_expired_set r myset 400 $live_ttl $live_plain]

            for {set i 0} {$i < 50} {incr i} {
                assert_all_live [list [r SRANDMEMBER myset]] $live_members
                foreach count {1 -1} {
                    set got [r SRANDMEMBER myset $count]
                    assert_equal 1 [llength $got]
                    assert_all_live $got $live_members
                }
            }
            foreach count {2 7 19 20 25 500} {
                set got [r SRANDMEMBER myset $count]
                assert_equal [expr {min($count, 20)}] [llength $got]
                assert_equal [llength $got] [llength [lsort -unique $got]]
                assert_all_live $got $live_members
                set got [r SRANDMEMBER myset -$count]
                assert_equal $count [llength $got]
                assert_all_live $got $live_members
            }

            # A read reclaims nothing, so every hidden member is still stored.
            assert_equal 420 [r SCARD myset]
            assert_equal 20 [llength [r SMEMBERS myset]]
            assert_equal 1 [get_keys_with_volatile_items r]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "SRANDMEMBER is unbiased over the live members of a mostly-expired set - $live_kind live" {
            flush_and_disable_active_expiry
            set live_members [make_mostly_expired_set r myset 400 $live_ttl $live_plain]

            # Every form reaches different selection code: single picks, picks with
            # repeats, and distinct picks from deduplicated draws. 19 degrees of
            # freedom over 20 live members; the cutoff is far enough above the 5%
            # critical value that no run trips it by chance and far enough below a
            # skewed draw that a member the sampler favours fails.
            set got {}
            for {set i 0} {$i < 2000} {incr i} { lappend got [r SRANDMEMBER myset] }
            assert_all_live $got $live_members
            assert_lessthan [chi_square_value $got] 60

            set got [r SRANDMEMBER myset -4000]
            assert_equal 4000 [llength $got]
            assert_all_live $got $live_members
            assert_equal [lsort $live_members] [lsort -unique $got]
            assert_lessthan [chi_square_value $got] 60

            foreach count {2 12} {
                set got {}
                for {set i 0} {$i < 4000 / $count} {incr i} {
                    lappend got {*}[r SRANDMEMBER myset $count]
                }
                assert_all_live $got $live_members
                assert_lessthan [chi_square_value $got] 60
            }

            # Drawing reclaims nothing.
            assert_equal 420 [r SCARD myset]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}
    }

    test "SRANDMEMBER returns the one live member of a mostly-expired set" {
        flush_and_disable_active_expiry
        foreach {live_ttl live_plain} {1 0 0 1} {
            set live_members [make_mostly_expired_set r myset 400 $live_ttl $live_plain]
            set only [lindex $live_members 0]
            assert_equal $only [r SRANDMEMBER myset]
            assert_equal $only [r SRANDMEMBER myset 1]
            assert_equal $only [r SRANDMEMBER myset 5]
            assert_equal [lrepeat 5 $only] [r SRANDMEMBER myset -5]
            assert_equal 401 [r SCARD myset]
        }
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test "SRANDMEMBER over an all-hidden set answers empty without reclaiming" {
        flush_and_disable_active_expiry
        make_mostly_expired_set r myset 400 0 0
        assert_equal {} [r SRANDMEMBER myset]
        assert_equal {} [r SRANDMEMBER myset 1]
        assert_equal {} [r SRANDMEMBER myset 500]
        assert_equal {} [r SRANDMEMBER myset -500]
        assert_equal 400 [r SCARD myset]
        assert_equal 1 [get_keys_with_volatile_items r]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test "SRANDMEMBER with a count at the live population returns exactly it" {
        flush_and_disable_active_expiry
        foreach {live_ttl live_plain} {20 0 0 20} {
            set live_members [make_mostly_expired_set r myset 400 $live_ttl $live_plain]
            assert_equal [lsort $live_members] [lsort [r SRANDMEMBER myset 20]]
        }
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    foreach {live_kind live_ttl live_plain} {volatile 20 0 persistent 0 20 mixed 10 10} {
        test "SPOP with a small count over a mostly-expired set pops live members only - $live_kind live" {
            flush_and_disable_active_expiry
            foreach count {1 2 12} {
                set live_members [make_mostly_expired_set r myset 400 $live_ttl $live_plain]
                set popped [r SPOP myset $count]
                assert_equal $count [llength $popped]
                assert_equal $count [llength [lsort -unique $popped]]
                assert_all_live $popped $live_members

                # Only the returned live members left; the hidden ones are still
                # stored and still hidden.
                assert_equal [expr {420 - $count}] [r SCARD myset]
                assert_equal [expr {20 - $count}] [llength [r SMEMBERS myset]]
                foreach m $popped { assert_equal 0 [r SISMEMBER myset $m] }
                assert_equal 1 [r EXISTS myset]
                assert_equal 1 [get_keys_with_volatile_items r]
            }
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}
    }

    test "SPOP over an all-hidden set returns empty and keeps every hidden member" {
        flush_and_disable_active_expiry
        make_mostly_expired_set r myset 400 0 0
        assert_equal {{} 0 0} [capture_mutation_signals myset {
            assert_equal {} [r SPOP myset 1]
            assert_equal {} [r SPOP myset 500]
        }]
        assert_equal 400 [r SCARD myset]
        assert_equal 1 [r EXISTS myset]
        assert_equal 1 [get_keys_with_volatile_items r]
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test "SPOP with a count of 1 pops the one live member of a mostly-expired set" {
        flush_and_disable_active_expiry
        foreach {live_ttl live_plain} {1 0 0 1} {
            set live_members [make_mostly_expired_set r myset 400 $live_ttl $live_plain]
            assert_equal [lindex $live_members 0] [r SPOP myset 1]
            assert_equal 400 [r SCARD myset]
            assert_equal {} [r SMEMBERS myset]
            assert_equal {} [r SPOP myset 1]
        }
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    # Pops reach the same selection code as reads, plus a pass that removes the
    # members it selects as it goes, so each count is checked on fresh copies.
    foreach {live_kind live_ttl live_plain} {volatile 20 0 persistent 0 20 mixed 10 10} {
        test "SPOP is unbiased over the live members of a mostly-expired set - $live_kind live" {
            flush_and_disable_active_expiry
            set live_members [make_mostly_expired_set r src 400 $live_ttl $live_plain]
            foreach count {1 5 19} {
                set got {}
                for {set i 0} {$i < 2000 / $count} {incr i} {
                    r DEL myset
                    r COPY src myset
                    set popped [r SPOP myset $count]
                    assert_equal $count [llength [lsort -unique $popped]]
                    lappend got {*}$popped
                }
                assert_all_live $got $live_members
                assert_equal [lsort $live_members] [lsort -unique $got]
                assert_lessthan [chi_square_value $got] 60
                # Only the popped members left the copy; its hidden ones stayed.
                assert_equal [expr {420 - $count}] [r SCARD myset]
            }
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}
    }

    # More live members than the expiry index collects for an answer, whether
    # or not they all carry a TTL.
    test "SRANDMEMBER and SPOP serve a mostly-expired set with many live members" {
        flush_and_disable_active_expiry
        foreach {live_ttl live_plain} {600 0 300 300} {
            set live_members [make_mostly_expired_set r src 3000 $live_ttl $live_plain]
            assert_all_live [list [r SRANDMEMBER src]] $live_members
            foreach count {1 10 300 599 600 700} {
                set got [r SRANDMEMBER src $count]
                assert_equal [expr {min($count, 600)}] [llength [lsort -unique $got]]
                assert_all_live $got $live_members
                set got [r SRANDMEMBER src -$count]
                assert_equal $count [llength $got]
                assert_all_live $got $live_members
            }
            foreach count {1 10 300 595 599 600 3600 5000} {
                r DEL myset
                r COPY src myset
                set popped [r SPOP myset $count]
                set expected [expr {min($count, 600)}]
                assert_equal $expected [llength [lsort -unique $popped]]
                assert_all_live $popped $live_members
                assert_equal [expr {3600 - $expected}] [r SCARD myset]
                assert_equal [expr {600 - $expected}] [llength [r SMEMBERS myset]]
                assert_equal 1 [r EXISTS myset]
            }
            assert_equal 3600 [r SCARD src]
        }
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}
}

# A set operand of a sorted set operation is iterated, and its members are handed
# to the other operands as lookup keys. Both are places where the set's own
# member metadata and its hidden members can leak out.

# One live and one hidden member, plus the sorted set operands that leave each
# store command with exactly the live member to store. None of them names the
# hidden member, so a command that sees it stores it.
proc seed_hidden_member_store_sources {r} {
    $r SADD s{t} live gone
    make_members_expired $r s{t} {gone}
    # Larger than the set, so the set is the operand the intersection iterates.
    $r ZADD z{t} 1 live 1 gone 1 extra
    $r ZADD other{t} 1 extra
    $r ZADD dst{t} 1 stale
}

# {store command, the score its single stored member gets, its operands}
set ::hidden_member_stores {
    ZUNIONSTORE 1 {1 s{t}}
    ZINTERSTORE 2 {2 s{t} z{t}}
    ZDIFFSTORE 1 {2 s{t} other{t}}
}

start_server {tags {"setexpire external:skip"}} {
    foreach encoding {listpack hashtable} {
        test "A set source keeps its member TTLs across the sorted set operations - $encoding" {
            r FLUSHALL
            use_set_encoding $encoding
            r SADDEX s{t} EX $::live_member_ttl MEMBERS 2 m1 m2
            assert_encoding $encoding s{t}
            r ZADD z{t} 1 m1 1 m2 1 other

            r ZUNION 1 s{t}
            r ZUNIONSTORE u{t} 1 s{t}
            r ZINTERSTORE i{t} 2 s{t} z{t}
            r ZINTERCARD 2 s{t} z{t}
            r ZDIFF 2 s{t} z{t}

            assert_range [set_member_ttl r s{t} m1] 90 $::live_member_ttl
            assert_range [set_member_ttl r s{t} m2] 90 $::live_member_ttl
        }

        test "A set source hides its expired members from the sorted set operations - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            r SADD s{t} live gone
            make_members_expired r s{t} {gone}
            assert_encoding $encoding s{t}
            r ZADD z{t} 1 live 1 gone 1 extra
            r ZADD one{t} 1 live
            r ZADD probe{t} 1 gone

            # The set is the operand being iterated: the only one, or the smallest.
            assert_equal {live} [r ZUNION 1 s{t}]
            assert_equal 1 [r ZUNIONSTORE u{t} 1 s{t}]
            assert_equal {live} [r ZRANGE u{t} 0 -1]
            assert_equal 1 [r ZINTERCARD 2 s{t} z{t}]
            assert_equal 1 [r ZINTERSTORE i{t} 2 s{t} z{t}]
            assert_equal {live} [r ZRANGE i{t} 0 -1]
            assert_equal {} [r ZDIFF 2 s{t} one{t}]

            # The set is the larger operand: the one searched, not the one iterated.
            assert_equal 1 [r ZINTERCARD 2 one{t} s{t}]
            assert_equal 0 [r ZINTERCARD 2 probe{t} s{t}]
            assert_equal 0 [r ZINTERSTORE i{t} 2 probe{t} s{t}]
            assert_equal {gone} [r ZDIFF 2 probe{t} s{t}]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        foreach {cmd score operands} $::hidden_member_stores {
            test "$cmd over a set source with hidden members propagates its result - $encoding" {
                flush_and_disable_active_expiry
                use_set_encoding $encoding
                seed_hidden_member_store_sources r
                assert_encoding $encoding s{t}

                set repl [attach_to_replication_stream]
                assert_equal 1 [r $cmd dst{t} {*}$operands]

                # Replaying the command would read the hidden member, which is
                # live on a replica, so only the result itself may be sent.
                assert_replication_stream $repl [subst {
                    {multi}
                    {select *}
                    {unlink dst{t}}
                    {zadd dst{t} $score live}
                    {exec}
                }]
                assert_equal {} [read_from_replication_stream $repl]
                close_replication_stream $repl
                assert_equal {live} [r ZRANGE dst{t} 0 -1]
                r DEBUG SET-ACTIVE-EXPIRE 1
            } {OK} {needs:debug}
        }

        test "ZINTERSTORE over a set source of only hidden members propagates one UNLINK - $encoding" {
            flush_and_disable_active_expiry
            use_set_encoding $encoding
            r SADD s{t} gone
            make_members_expired r s{t} {gone}
            r ZADD z{t} 1 gone 1 extra

            set repl [attach_to_replication_stream]
            # A replica sees the hidden member as live, so an absent destination
            # must receive the deletion rather than the command it would recompute.
            assert_equal 0 [r ZINTERSTORE dst{t} 2 s{t} z{t}]
            r ZADD dst{t} 1 stale
            assert_equal 0 [r ZINTERSTORE dst{t} 2 s{t} z{t}]

            assert_replication_stream $repl {
                {select *}
                {unlink dst{t}}
                {zadd dst{t} 1 stale}
                {unlink dst{t}}
            }
            assert_equal {} [read_from_replication_stream $repl]
            close_replication_stream $repl
            assert_equal 0 [r EXISTS dst{t}]
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}
    }

    # The result is replayed from the destination, whose listpack integer members
    # and ordered index each take their own branch.
    foreach {zencoding zentries} {listpack 128 btree 0} {
        test "ZUNIONSTORE over a set source with hidden members propagates integer members - $zencoding" {
            flush_and_disable_active_expiry
            r CONFIG SET zset-max-listpack-entries $zentries
            r SADD s{t} 1 2 3 gone
            make_members_expired r s{t} {gone}

            set repl [attach_to_replication_stream]
            assert_equal 3 [r ZUNIONSTORE dst{t} 1 s{t}]
            assert_encoding $zencoding dst{t}

            assert_replication_stream $repl {
                {multi}
                {select *}
                {unlink dst{t}}
                {zadd dst{t} 1 1 1 2 1 3}
                {exec}
            }
            close_replication_stream $repl
            r CONFIG SET zset-max-listpack-entries 128
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}
    }
}

# A replica holds the members the primary hides, so a store command it recomputes
# reads members the primary did not.
start_server {tags {"setexpire needs:repl external:skip"}} {
    start_server {} {
        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        set replica [srv 0 client]

        foreach encoding {listpack hashtable} {
            foreach {cmd score operands} $::hidden_member_stores {
                test "$cmd over a set source with hidden members stores the same result on the replica - $encoding" {
                    $primary FLUSHALL
                    $primary DEBUG SET-ACTIVE-EXPIRE 0
                    use_set_encoding $encoding $primary
                    use_set_encoding $encoding $replica
                    attach_replica $primary $replica $primary_host $primary_port
                    seed_hidden_member_store_sources $primary
                    wait_for_ofs_sync $primary $replica
                    # The comparison only means something while the replica still
                    # holds the member the primary is hiding.
                    assert_equal 2 [$replica SCARD s{t}]

                    assert_equal 1 [$primary $cmd dst{t} {*}$operands]
                    assert_equal {live} [$primary ZRANGE dst{t} 0 -1]
                    wait_for_ofs_sync $primary $replica
                    assert_equal [$primary ZRANGE dst{t} 0 -1 WITHSCORES] \
                        [$replica ZRANGE dst{t} 0 -1 WITHSCORES]

                    $primary DEBUG SET-ACTIVE-EXPIRE 1
                } {OK} {needs:debug}
            }
        }
    }
}
