start_server {tags {"payload-histogram"}} {
    r config set payload-histogram-views "32kb 64kb"
    r config set payload-histogram-factor 2

    test {PAYLOAD HISTOGRAM empty when tracking disabled} {
        r set a b
        set res [dict create {*}[r payload histogram]]
        assert {[dict size [dict get $res read]] == 0}
        assert {[dict size [dict get $res write]] == 0}
    }

    test {PAYLOAD HISTOGRAM counts increase when enabled} {
        r config set payload-tracking yes
        r set foo [string repeat a 64]
        r get foo

        set res [dict create {*}[r payload histogram]]
        set read_view [dict get $res read 32768]
        set calls1 [dict get $read_view calls]
        set buckets1 [dict get $read_view histogram_bytes]
        set sum1 0
        dict for {k v} $buckets1 {incr sum1 $v}
        assert {$sum1 == $calls1}

        r set bar [string repeat b 128]

        set res2 [dict create {*}[r payload histogram]]
        set read_view2 [dict get $res2 read 32768]
        set calls2 [dict get $read_view2 calls]
        set buckets2 [dict get $read_view2 histogram_bytes]
        set sum2 0
        dict for {k v} $buckets2 {incr sum2 $v}
        assert {$sum2 == $calls2}

        assert {$calls2 > $calls1}
    }

    test {PAYLOAD HISTOGRAM direction filter} {
        set res [dict create {*}[r payload histogram read]]
        assert {[dict size $res] == 1}
        assert {[dict exists $res read]}
        assert {![dict exists $res write]}
    }
}
