# Test that Sentinel ignores hello messages with invalid epochs.
source "../tests/includes/init-tests.tcl"

test "Sentinel ignores hello messages with a negative epoch" {
    set fake_runid 0123456789abcdef0123456789abcdef01234567
    set master_ip [get_instance_attrib valkey $master_id host]
    set master_port [get_instance_attrib valkey $master_id port]

    # Negative current epoch, then negative primary config epoch.
    R $master_id publish __sentinel__:hello "127.0.0.1,31098,$fake_runid,-1,mymaster,$master_ip,$master_port,0"
    R $master_id publish __sentinel__:hello "127.0.0.1,31098,$fake_runid,1,mymaster,$master_ip,$master_port,-1"
    after 1000

    foreach_sentinel_id id {
        # The whole message was dropped, so the sender was not added.
        assert_no_match "*$fake_runid*" [S $id SENTINEL SENTINELS mymaster]

        # The current epoch did not wrap around: a vote for a normal epoch
        # is still granted.
        set myid [S $id SENTINEL MYID]
        set result [S $id SENTINEL IS-PRIMARY-DOWN-BY-ADDR $master_ip $master_port 100 $myid]
        assert_equal $myid [lindex $result 1]
        assert_equal {100} [lindex $result 2]
    }
}
