# Test that Sentinel ignores hello messages with invalid epochs.
source "../tests/includes/init-tests.tcl"

test "Sentinel ignores hello messages with a negative epoch" {
    set fake_runid 0123456789abcdef0123456789abcdef01234567
    set master_ip [get_instance_attrib valkey $master_id host]
    set master_port [get_instance_attrib valkey $master_id port]

    foreach_sentinel_id id {
        # PUBLISH on a Sentinel processes the hello before replying, so no
        # wait is needed. Negative current epoch, negative primary config
        # epoch, and a negative epoch below LLONG_MIN.
        foreach epochs {{-1 0} {1 -1} {-9223372036854775809 0}} {
            lassign $epochs cur cfg
            S $id publish __sentinel__:hello "127.0.0.1,31098,$fake_runid,$cur,mymaster,$master_ip,$master_port,$cfg"
        }

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
