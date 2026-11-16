# A Sentinel must never list itself, or a stale identity at its own address, as a peer.

source "../tests/includes/init-tests.tcl"

set num_sentinels [llength $::sentinel_instances]

proc sentinel_runids {id} {
    set runids {}
    foreach sentinel [S $id SENTINEL SENTINELS mymaster] {
        lappend runids [dict get $sentinel runid]
    }
    return $runids
}

proc own_address_hello {id runid} {
    set addr [S $id SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]
    set host [get_instance_attrib sentinel $id host]
    set port [get_instance_attrib sentinel $id port]
    return "$host,$port,$runid,0,mymaster,[lindex $addr 0],[lindex $addr 1],0"
}

proc peer_port {id runid} {
    foreach peer [S $id SENTINEL SENTINELS mymaster] {
        if {[dict get $peer runid] eq $runid} { return [dict get $peer port] }
    }
    return -1
}

test "Sentinel ignores its own hello sent with PUBLISH" {
    set myid [S 0 SENTINEL MYID]
    S 0 PUBLISH __sentinel__:hello [own_address_hello 0 $myid]

    assert_equal -1 [lsearch [sentinel_runids 0] $myid]
    assert_equal [expr {$num_sentinels-1}] [dict get [S 0 SENTINEL PRIMARY mymaster] num-other-sentinels]
}

test "Sentinel ignores its own runid published with another address" {
    set myid [S 0 SENTINEL MYID]
    set addr [S 0 SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]
    # A port nothing listens on, so the hello cannot really be from us.
    set other_port [expr {[get_instance_attrib sentinel 0 port] + 1000}]
    S 0 PUBLISH __sentinel__:hello "127.0.0.1,$other_port,$myid,0,mymaster,[lindex $addr 0],[lindex $addr 1],0"

    assert_equal -1 [lsearch [sentinel_runids 0] $myid]
    assert_equal [expr {$num_sentinels-1}] [dict get [S 0 SENTINEL PRIMARY mymaster] num-other-sentinels]
}

test "Sentinel removes a stale identity announced at its own address" {
    S 0 SENTINEL RESET mymaster
    verify_sentinel_auto_discovery

    # A restarted Sentinel can receive a late hello from its previous
    # incarnation, which announced the same address under another runid.
    set myid [S 0 SENTINEL MYID]
    set stale_runid [string repeat "a" 40]
    S 0 PUBLISH __sentinel__:hello [own_address_hello 0 $stale_runid]
    assert_morethan_equal [lsearch [sentinel_runids 0] $stale_runid] 0

    # Sentinel 0 now sends hellos to the stale peer, which is itself.
    wait_for_condition 100 100 {
        [lsearch [sentinel_runids 0] $stale_runid] == -1 &&
        [dict get [S 0 SENTINEL PRIMARY mymaster] num-other-sentinels] == ($num_sentinels-1)
    } else {
        fail "Sentinel 0 still tracks a peer at its own address: [sentinel_runids 0]"
    }
    assert_equal -1 [lsearch [sentinel_runids 0] $myid]
}

test "Sentinel ignores its own runid in a known-sentinel config line" {
    # An older version could save its own runid as a known-sentinel. Load it
    # at an address nothing answers on, so no hello can remove it.
    set myid [S 0 SENTINEL MYID]
    set port [expr {[get_instance_attrib sentinel 0 port] + 1000}]
    set cfgfile [file join "sentinel_0" "sentinel.conf"]
    kill_instance sentinel 0
    set fd [open $cfgfile a]
    puts $fd "sentinel known-sentinel mymaster 127.0.0.1 $port $myid"
    close $fd
    restart_instance sentinel 0
    verify_sentinel_auto_discovery

    assert_equal -1 [lsearch [sentinel_runids 0] $myid]
    assert_equal [expr {$num_sentinels-1}] [dict get [S 0 SENTINEL PRIMARY mymaster] num-other-sentinels]

    S 0 SENTINEL FLUSHCONFIG
    set fd [open $cfgfile r]
    set content [read $fd]
    close $fd
    assert_equal -1 [string first "known-sentinel mymaster 127.0.0.1 $port $myid" $content]
}

test "Sentinel removes a stale runid at its own address loaded from known-sentinel" {
    set stale [string repeat b 40]
    set host [get_instance_attrib sentinel 0 host]
    set port [get_instance_attrib sentinel 0 port]
    set cfgfile [file join "sentinel_0" "sentinel.conf"]
    kill_instance sentinel 0
    set fd [open $cfgfile a]
    puts $fd "sentinel known-sentinel mymaster $host $port $stale"
    close $fd
    restart_instance sentinel 0

    wait_for_condition 150 100 {
        [lsearch [sentinel_runids 0] $stale] == -1 &&
        [dict get [S 0 SENTINEL PRIMARY mymaster] num-other-sentinels] == ($num_sentinels-1)
    } else {
        fail "Sentinel 0 still tracks the stale peer: [sentinel_runids 0]"
    }
}

test "Sentinel replaces a peer when a new runid announces its address" {
    set addr [S 0 SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]
    set old [S 1 SENTINEL MYID]
    set host [get_instance_attrib sentinel 1 host]
    set port [get_instance_attrib sentinel 1 port]
    set new [string repeat c 40]
    S 0 PUBLISH __sentinel__:hello "$host,$port,$new,0,mymaster,[lindex $addr 0],[lindex $addr 1],0"
    set runids [sentinel_runids 0]
    assert_equal -1 [lsearch $runids $old]
    assert_morethan_equal [lsearch $runids $new] 0
    # Sentinel 1's next real hello takes its address back the same way.
    wait_for_condition 100 100 {
        [lsearch [sentinel_runids 0] $old] >= 0 && [lsearch [sentinel_runids 0] $new] == -1
    } else {
        fail "Sentinel 1 did not take its address back: [sentinel_runids 0]"
    }
}

test "Sentinel follows a peer's address switch" {
    set addr [S 0 SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]
    set peer [S 1 SENTINEL MYID]
    set host [get_instance_attrib sentinel 1 host]
    set port [get_instance_attrib sentinel 1 port]
    set moved [expr {$port + 1000}]
    S 0 PUBLISH __sentinel__:hello "$host,$moved,$peer,0,mymaster,[lindex $addr 0],[lindex $addr 1],0"
    assert_equal $moved [peer_port 0 $peer]
    wait_for_condition 100 100 {
        [peer_port 0 $peer] == $port
    } else {
        fail "Sentinel 1 did not switch back to port $port"
    }
}
