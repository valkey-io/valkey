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

test "Sentinel ignores its own hello sent with PUBLISH" {
    set myid [S 0 SENTINEL MYID]
    S 0 PUBLISH __sentinel__:hello [own_address_hello 0 $myid]

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
