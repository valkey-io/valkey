proc ensure_cron {} {
    set before [s expired_keys]
    r setex expired_key 1 v
    wait_for_condition 50 100 {
        [s expired_keys] > $before
    } else {
        fail "serverCron never runs"
    }

    assert_equal [expr {$before + 1}] [s expired_keys]
}

proc check_rewrites_new {count} {
    ensure_cron

    # Wait for new rewrite to finish
    wait_for_condition 50 100 {
        [s aof_rewrite_in_progress] eq 0 &&
        [s aof_rewrites] eq $count
    } else {
        fail "Rewrite behaviour is unexpected: progress=[s aof_rewrite_in_progress], rewrites=[s aof_rewrites], expected=$count"
    }
}

proc setup {aof_max_size {min_size 64mb}} {
    r config set hz 10  ; # serverCron runs every ~100ms
    r config set auto-aof-rewrite-percentage 0 ; # disable auto-rewrite
    r config set auto-aof-rewrite-max-size $aof_max_size
    r config set auto-aof-rewrite-min-size $min_size
    r config set appendonly yes ; # enable AOF

    # Wait for initial serverCron to schedule and execute rewrite
    check_rewrites_new 1
}

proc check_rewrites_old {log_count_before aof_rewrites} {
    ensure_cron

    set log_count_after [count_log_message 0 "*Background append only file rewriting started*"]
    # Should still be the same (no new rewrite triggered)
    assert {$log_count_after == $log_count_before}
    assert {[s aof_rewrite_in_progress] == 0}
    assert {[s aof_rewrites] == $aof_rewrites}
}

proc cleanup {} {
    r config set auto-aof-rewrite-max-size 0
    r flushall
}

proc fill_aof_to_size {target_size_mb {repeated_keys_size 0}} {
    # Save current auto-aof-rewrite-max-size and disable it during fill
    set saved_max_size [lindex [r config get auto-aof-rewrite-max-size] 1]
    r config set auto-aof-rewrite-max-size 0

    set target_bytes [expr {$target_size_mb * 1024 * 1024}]
    set current_size [status r aof_current_size]
    set iterations 0

    while {$current_size < $target_bytes && $iterations < 100000} {
        if {$repeated_keys_size > 0} {
            # Overwrite only N keys repeatedly to create redundancy
            r set key[expr {$iterations % $repeated_keys_size}] [string repeat "x" 1000]
        } else {
            # Use unique keys
            r set fillkey$iterations [string repeat "x" 1000]
        }
        incr iterations
        if {$iterations % 100 == 0} {
            set current_size [status r aof_current_size]
        }
    }

    # Restore original auto-aof-rewrite-max-size
    r config set auto-aof-rewrite-max-size $saved_max_size

    return $current_size
}

start_server {tags {"external:skip" "slow"}} {
    test "auto-aof-rewrite-max-size triggers successful rewrite and reduces size" {
        setup 102400 64kb

        # Fill AOF with redundant commands that will compress during rewrite
        # Overwrite only 1000 keys repeatedly to create AOF bloat
        fill_aof_to_size 2 1000

        set size_before [status r aof_current_size]
        assert {$size_before > 102400}

        # Wait for serverCron to schedule and execute rewrite
        check_rewrites_new 2

        set size_after [status r aof_current_size]
        assert {$size_after < $size_before}

        cleanup
    }
}

start_server {tags {"external:skip" "slow"}} {
    test "auto-aof-rewrite-max-size prevents eternal rewrite loop when base >= max" {
        setup 51200 32kb

        # Create dataset that won't compress below 50KB using fill helper
        fill_aof_to_size 2

        # Wait for serverCron to schedule and execute rewrite
        check_rewrites_new 2

        set base_size [s aof_base_size]
        assert {$base_size >= 51200}

        set log_count_before [count_log_message 0 "*Background append only file rewriting started*"]

        # Add more data
        fill_aof_to_size 0.1

        # Wait a bit for serverCron cycles
        check_rewrites_old $log_count_before 2

        cleanup
    }
}

start_server {tags {"external:skip" "slow"}} {
    test "auto-aof-rewrite-max-size=0 disables size-based rewriting" {
        setup 0 0

        set log_count_before [count_log_message 0 "*Background append only file rewriting started*"]

        # Fill AOF with data
        fill_aof_to_size 2

        # Wait a bit for serverCron cycles
        check_rewrites_old $log_count_before 1

        cleanup
    }
}

start_server {tags {"external:skip" "slow"}} {
    test "user-initiated BGREWRITEAOF works independently of auto-aof-rewrite-max-size" {
        setup 10485760

        # Add some data but not enough to trigger auto-aof-rewrite-max-size
        fill_aof_to_size 1

        # Manually trigger rewrite
        assert {[r bgrewriteaof] eq "Background append only file rewriting started"}

        # Should start rewrite even though auto-aof-rewrite-max-size not exceeded
        check_rewrites_new 2

        cleanup
    }
}

start_server {tags {"external:skip" "slow"}} {
    test "auto-aof-rewrite-max-size checks all conditions in serverCron" {
        setup 524288 262144

        set log_count_before [count_log_message 0 "*Background append only file rewriting started*"]

        # Fill to just above min-size but below max-size (0.3MB)
        fill_aof_to_size 0.3

        set current_size [status r aof_current_size]
        assert {$current_size > 262144 && $current_size < 524288}

        # Should NOT trigger rewrite (not at max-size yet)
        check_rewrites_old $log_count_before 1

        # Now exceed max-size (0.6MB)
        fill_aof_to_size 0.6

        # Should trigger rewrite now
        check_rewrites_new 2

        cleanup
    }
}
