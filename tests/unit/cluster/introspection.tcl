start_cluster 1 0 {tags {external:skip cluster}} {
    test "CONFIG REWRITE databases" {
        assert_equal 16 [lindex [r config get databases] 1]
        r config rewrite
        restart_server 0 true false
        assert_equal 16 [lindex [r config get databases] 1]
    }
}
