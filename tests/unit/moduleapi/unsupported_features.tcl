start_cluster 3 0 {tags {logreqres:skip external:skip cluster} overrides {enable-module-command yes}} {
    set testmodule [file normalize tests/modules/unsupported_features.so]

    set node0_id [R 0 CLUSTER MYID]
    set node1_id [R 1 CLUSTER MYID]
    set node2_id [R 2 CLUSTER MYID]

    test "ASM blocked by module source" {
        assert_match "OK" [R 2 MODULE LOAD $testmodule]

        assert_error "ERR The module unsupported_features does not support atomic slot migrations*" {R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id}

        assert_match "OK" [R 2 MODULE UNLOAD unsupported_features]
    }

    test "ASM blocked by module target" {
        assert_match "OK" [R 0 MODULE LOAD $testmodule]

        assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]

        wait_for_condition 50 100 {
            [dict get [lindex [R 2 CLUSTER GETSLOTMIGRATIONS] 0] state] eq "failed"
        } else {
            fail "Slot migration did not fail despite target having module that did not support it"
        }

        assert_match "Received error during handshake to target: -ERR The module unsupported_features does not support atomic slot migrations*" [dict get [lindex [R 2 CLUSTER GETSLOTMIGRATIONS] 0] message]

        assert_match "OK" [R 0 MODULE UNLOAD unsupported_features]
    }
}
