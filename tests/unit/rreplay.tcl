start_server {tags {"rreplay"}} {
    test {RREPLAY command is registered} {
        set cmdinfo [r command info rreplay]
        assert {[llength $cmdinfo] == 1}
        set info [lindex $cmdinfo 0]
        assert_equal rreplay [lindex $info 0]
        assert_equal -5 [lindex $info 1]
    }

    test {RREPLAY is rejected for normal clients} {
        assert_error "*replication link*" {
            r rreplay 0123456789abcdef0123456789abcdef01234567 0 1 set k v
        }
    }
}
