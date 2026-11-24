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

        # open primary replication stream so we can assert what gets propagated.
        set repl [attach_to_replication_stream]

        # flush slot key
        R 0 CLUSTER FLUSHSLOT $key_slot SYNC
        assert_equal [R 0 CLUSTER COUNTKEYSINSLOT $key_slot] 0

        # assert flush slot propagated to replica
        assert_replication_stream $repl {
            {select *}
            {cluster*FLUSHSLOT*}
        }

        close_replication_stream $repl
    }
}
start_cluster 2 0 {tags {external:skip cluster}} {
    set key_slot [R 0 CLUSTER KEYSLOT FC]

    # The test config enables keyspace events, which makes CLUSTER FLUSHSLOT
    # visit every key. Disable them so the WATCH and blocked client tests cover
    # the path that does not.
    R 0 config set notify-keyspace-events ""

    test "Flush slot command aborts EXEC of clients watching keys in the slot" {
        R 0 set "{FC}-watched" "value"
        R 0 set "{06S}-watched" "value"

        # A watched key in the flushed slot, a watched key in another slot and
        # a watched key in the flushed slot that does not exist.
        set watch_deleted [valkey_client_by_addr [srv 0 host] [srv 0 port]]
        set watch_other_slot [valkey_client_by_addr [srv 0 host] [srv 0 port]]
        set watch_missing [valkey_client_by_addr [srv 0 host] [srv 0 port]]
        $watch_deleted watch "{FC}-watched"
        $watch_other_slot watch "{06S}-watched"
        $watch_missing watch "{FC}-missing"

        R 0 CLUSTER FLUSHSLOT $key_slot SYNC

        # Only the client watching a deleted key has its transaction aborted.
        foreach {client expected} [list $watch_deleted {} $watch_other_slot PONG $watch_missing PONG] {
            $client multi
            $client ping
            assert_equal $expected [$client exec]
            $client close
        }
        R 0 del "{06S}-watched"
    }

    test "Flush slot command aborts EXEC when more keys are watched than the slot holds" {
        R 0 set "{FC}-watched" "value"

        # More keys are watched than the flushed slot holds, so the flush
        # walks the slot instead of the watched keys.
        set watch_deleted [valkey_client_by_addr [srv 0 host] [srv 0 port]]
        set watch_other_slot [valkey_client_by_addr [srv 0 host] [srv 0 port]]
        $watch_deleted watch "{FC}-watched"
        $watch_other_slot watch "{06S}-a" "{06S}-b" "{06S}-c"

        R 0 CLUSTER FLUSHSLOT $key_slot SYNC

        foreach {client expected} [list $watch_deleted {} $watch_other_slot PONG] {
            $client multi
            $client ping
            assert_equal $expected [$client exec]
            $client close
        }
    }

    test "Flush slot command wakes up clients blocked on a stream in the slot" {
        R 0 xadd "{FC}-stream" * field value
        R 0 xgroup create "{FC}-stream" group $

        set rd [valkey_deferring_client_by_addr [srv 0 host] [srv 0 port]]
        $rd xreadgroup group group consumer block 0 streams "{FC}-stream" >
        wait_for_blocked_clients_count 1 100 10 0

        R 0 CLUSTER FLUSHSLOT $key_slot SYNC

        wait_for_blocked_clients_count 0 100 10 0
        assert_error "*NOGROUP*" {$rd read}
        $rd close
    }

    test "Flush slot command invalidates keys read by tracking clients" {
        R 0 set "{FC}-tracked" "value"

        set rd [valkey_deferring_client_by_addr [srv 0 host] [srv 0 port]]
        $rd hello 3
        $rd read
        $rd client tracking on
        $rd read
        $rd get "{FC}-tracked"
        $rd read

        R 0 CLUSTER FLUSHSLOT $key_slot SYNC

        assert_equal [list invalidate [list "{FC}-tracked"]] [$rd read]
        $rd close
    }

    test "Flush slot command sends keyspace del events" {
        R 0 config set notify-keyspace-events Eg
        R 0 set "{FC}-notified" "value"

        set rd [valkey_deferring_client_by_addr [srv 0 host] [srv 0 port]]
        $rd subscribe __keyevent@0__:del
        $rd read

        R 0 CLUSTER FLUSHSLOT $key_slot SYNC

        assert_equal [list message __keyevent@0__:del "{FC}-notified"] [$rd read]
        $rd close
        R 0 config set notify-keyspace-events ""
    }
}
