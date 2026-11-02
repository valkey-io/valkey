set testmodule [file normalize tests/modules/test_lazyfree.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "modules allocated memory can be reclaimed in the background" {
        set orig_mem [s used_memory]
        set rd [valkey_deferring_client]

        # LAZYFREE_THRESHOLD is 64
        $rd client reply off
        for {set i 0} {$i < 10000} {incr i} {
            $rd lazyfreelink.insert lazykey $i
            if {$i % 1000 == 0} {$rd flush}
        }
        $rd client reply on
        assert_equal OK [$rd read]

        assert {[r lazyfreelink.len lazykey] == 10000}

        set peak_mem [s used_memory]
        assert {[r unlink lazykey] == 1}
        assert {$peak_mem > $orig_mem+10000}
        wait_for_condition 50 100 {
            [s used_memory] < $peak_mem &&
            [s used_memory] < $orig_mem*2 &&
            [string match {*lazyfreed_objects:1*} [r info Memory]]
        } else {
            fail "Module memory is not reclaimed by UNLINK"
        }
    }
}

start_server {tags {"modules"} overrides {forkless-infrastructure-enabled yes bgsave-default-method forkless save ""}} {
    r module load $testmodule forkless

    test "FLUSHALL cancels a forkless bgsave that is still closing its file" {
        # Block the lazyfree BIO worker, so the close-and-rename job a
        # finished forkless save queues behind it cannot run yet.
        for {set i 0} {$i < 100} {incr i} {
            r lazyfreelink.insert lazykey $i
        }
        r lazyfreelink.holdfree 1
        r unlink lazykey

        r set stale v
        set loglines [count_log_lines 0]
        r bgsave
        wait_for_log_messages 0 {"*created background thread to perform snapshot file close and rename*"} $loglines 100 10

        r config set save "3600 1"
        r flushall
        r lazyfreelink.holdfree 0
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 0
        } else {
            fail "forkless bgsave did not finish"
        }

        # dump.rdb must still hold FLUSHALL's empty snapshot.
        r debug reload nosave
        assert_equal [r dbsize] 0
    } {} {needs:debug}
}
