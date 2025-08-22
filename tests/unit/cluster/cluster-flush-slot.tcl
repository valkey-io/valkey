start_cluster 2 2 {tags {external:skip cluster}} {
    test "SYNC Flush slot command" {
        set key_slot [R 0 CLUSTER KEYSLOT FC]
        set slot_keys_num [R 0 CLUSTER COUNTKEYSINSLOT $key_slot]

        # set key
        for {set i 0} {$i < 1000} {incr i} {
            R 0 set "{FC}-$i" "value"
        }
        set after_keys_num [expr {$slot_keys_num + 1000}]
        assert_equal [R 0 CLUSTER COUNTKEYSINSLOT $key_slot] $after_keys_num

        # flush slot key
        R 0 CLUSTER FLUSHSLOT $key_slot SYNC
        assert_equal [R 0 CLUSTER COUNTKEYSINSLOT $key_slot] 0
    }

    test "ASYNC Flush slot command" {
        set key_slot [R 0 CLUSTER KEYSLOT FC]
        set slot_keys_num [R 0 CLUSTER COUNTKEYSINSLOT $key_slot]

        # set key
        for {set i 0} {$i < 1000} {incr i} {
            R 0 set "{FC}-$i" "value"
        }
        set after_keys_num [expr {$slot_keys_num + 1000}]
        assert_equal [R 0 CLUSTER COUNTKEYSINSLOT $key_slot] $after_keys_num

        # flush slot key
        R 0 CLUSTER FLUSHSLOT $key_slot ASYNC
        assert_equal [R 0 CLUSTER COUNTKEYSINSLOT $key_slot] 0
    }
}

start_cluster 2 2 {tags {external:skip cluster}} {
    test "Flush slot command propagated to replica" {
        set key_slot [R 0 CLUSTER KEYSLOT FC]
        set slot_keys_num [R 0 CLUSTER COUNTKEYSINSLOT $key_slot]

        # set key
        for {set i 0} {$i < 1000} {incr i} {
            R 0 set "{FC}-$i" "value"
        }
        set after_keys_num [expr {$slot_keys_num + 1000}]
        assert_equal [R 0 CLUSTER COUNTKEYSINSLOT $key_slot] $after_keys_num

        # flush slot key
        R 0 CLUSTER FLUSHSLOT $key_slot SYNC
        assert_equal [R 0 CLUSTER COUNTKEYSINSLOT $key_slot] 0

        # assert flush slot propagated to replica
        set info [R 2 info commandSTATS]
        # not del cmd
        assert_no_match "*cmdstat_del*" $info
        # has flushslot cmd
        assert_match "*cmdstat_cluster|flushslot:calls=1*" $info
    }
}