set testmodule [file normalize tests/modules/blockedclient.so]

start_server {tags {"modules repl external:skip"} overrides {save {} forkless-infrastructure-enabled yes forkless-policy prefer-forkless repl-diskless-sync yes repl-diskless-sync-delay 0 repl-timeout 60}} {
    # Without handle-forkless the module blocks forkless saves and the sync forks.
    r module load $testmodule handle-forkless
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    $primary debug populate 50000 key 1000

    start_server {overrides {save {}}} {
        set replica [srv 0 client]
        set replica_pid [srv 0 pid]

        test {A module thread write during a forkless full sync does not crash the primary} {
            $replica replicaof $primary_host $primary_port
            wait_for_condition 500 10 {
                [s -1 rdb_bgsave_in_progress] == 1
            } else {
                fail "full sync did not start"
            }
            # Stall the transfer so the forkless iteration stays active.
            pause_process $replica_pid
            assert_equal 1 [s -1 rdb_bgsave_in_progress]
            assert_equal {forkless} [s -1 rdb_current_bgsave_type]

            # A replicated write ("!") from a module thread that holds the GIL.
            assert_equal {OK} [$primary do_bg_rm_call_format ! set foo bar]
            assert_equal {PONG} [$primary ping]

            resume_process $replica_pid
            wait_for_sync $replica
            wait_for_ofs_sync $primary $replica
            assert_equal {bar} [$replica get foo]
        }
    }
}
