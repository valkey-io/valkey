proc scan_interleaved {primary replica cmd args} {
    set cursor 0
    set keys {}
    set toggle [randomInt 2]
    while {1} {
        if {$toggle == 0} {
            set scan_result [$primary $cmd {*}$args $cursor]
        } else {
            set scan_result [$replica $cmd {*}$args $cursor]
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
    start_server {tags {"external:skip"}} {

        set fixed_seed [randstring 16 16 alpha]
        set shared_overrides [list appendonly no save "" hash-seed $fixed_seed activedefrag no hz 20]

        start_server [list overrides $shared_overrides] {
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]

            start_server [list overrides $shared_overrides] {
                set primary [srv -1 client]
                set replica [srv 0 client]

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

                foreach {cmd key} {hscan h sscan s zscan z} {
                    set keys [scan_interleaved $primary $replica $cmd $key]

                    if {$cmd eq "hscan" || $cmd eq "zscan"} {
                        set extracted_keys {}
                        foreach {k v} $keys {
                            lappend extracted_keys $k
                        }
                        set keys $extracted_keys
                    }
                    set keys [lsort -unique $keys]
                    assert_equal $n [llength $keys]
                }
            }
        }
    }
}