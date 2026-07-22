proc scan_interleaved {primary replica cmd {key ""} args} {
    set cursor 0
    set keys {}
    set toggle [randomInt 2]
    while {1} {
        if {$key != ""} {
            set cmd_args [list $key $cursor {*}$args]
        } else {
            set cmd_args [list $cursor {*}$args]
        }

        if {$toggle == 0} {
            set scan_result [$primary $cmd {*}$cmd_args]
        } else {
            set scan_result [$replica $cmd {*}$cmd_args]
        }
        lappend keys {*}[lindex $scan_result 1]
        if {[lindex $scan_result 0] eq 0} {
            break
        }
        set cursor [lindex $scan_result 0]
        set toggle [expr {1 - $toggle}]
    }
    return $keys
}

test {scan family consistency with configured hash seed} {
    set fixed_seed [randstring 16 16 alpha]
    set shared_overrides [list appendonly no save "" hash-seed $fixed_seed]

    start_multiple_servers 2 [list overrides $shared_overrides] {
        set primary [srv -1 client]
        set replica [srv 0 client]

        set primary_host [srv -1 host]
        set primary_port [srv -1 port]

        $primary flushall
        $replica replicaof $primary_host $primary_port
        wait_for_sync $replica

        set n 50
        for {set i 0} {$i < $n} {incr i} {
            $primary set "k:$i" x
            $primary hset h "f:$i" $i
            $primary sadd s "m:$i"
            $primary zadd z $i "m:$i"
        }

        wait_for_condition 200 50 {
            [$replica dbsize] == [$primary dbsize]
        } else {
            fail "replica did not catch up dbsize (primary=[$primary dbsize], replica=[$replica dbsize])"
        }

        set keys [scan_interleaved $primary $replica scan]
        set keys [lsort -unique $keys]
        assert_equal [expr {$n+3}] [llength $keys]

        foreach {cmd key extra} {hscan h {novalues} sscan s {} zscan z {noscores}} {
            set items [scan_interleaved $primary $replica $cmd $key {*}$extra]
            set items [lsort -unique $items]
            assert_equal $n [llength $items]
        }
    }
} {} {external:skip}

test {hash-seed uses full bytes including embedded NULL for seeding} {
    set seed_a {"valkey\x00AAAAA"}
    set seed_b {"valkey\x00BBBBB"}

    start_server [list overrides [list hash-seed $seed_a]] {
        start_server [list overrides [list hash-seed $seed_b]] {
            set primary [srv -1 client]
            set replica [srv 0 client]

            set primary_seed [lindex [$primary config get hash-seed] 1]
            set replica_seed [lindex [$replica config get hash-seed] 1]
            assert_not_equal $primary_seed $replica_seed

            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_sync $replica

            set n 16384
            for {set i 0} {$i < $n} {incr i} {
                $primary set "k:$i" x
            }

            wait_for_condition 200 50 {
                [$replica dbsize] == [$primary dbsize]
            } else {
                fail "replica did not catch up"
            }

            set keys [scan_interleaved $primary $replica scan]
            set keys [lsort -unique $keys]
            assert_not_equal $n [llength $keys]

            # Also use this opportunity to verify that the config rewrite works on NULL.
            $primary config rewrite
            restart_server -1 true false
            assert_equal [lindex [[srv -1 client] config get hash-seed] 1] $primary_seed
            $replica config rewrite
            restart_server 0 true false
            assert_equal [lindex [[srv 0 client] config get hash-seed] 1] $replica_seed
        }
    }
} {} {external:skip}
