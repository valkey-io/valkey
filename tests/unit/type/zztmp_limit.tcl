start_server {tags {"stream needs:debug"} overrides {appendonly yes stream-node-max-entries 10}} {
    test {XTRIM with ~ MAXLEN can propagate correctly} {
        for {set j 0} {$j < 100} {incr j} {
            r XADD mystream * xitem v
        }
        r XTRIM mystream MAXLEN ~ 85
        assert {[r xlen mystream] == 90}
        r config set stream-node-max-entries 1
        r debug loadaof
        r XADD mystream * xitem v
        incr j
        assert {[r xlen mystream] == 91}
        r flushall
    }

    test {XADD/XTRIM strip redundant LIMIT when rewriting for propagation} {
        set aof [get_last_incr_aof_path r]
        r config set stream-node-max-entries 10

        for {set j 0} {$j < 100} {incr j} {
            r XADD mystream * xitem v
        }
        assert_equal 100 [r xlen mystream]

        r XADD mystream MAXLEN ~ 55 LIMIT 30 * xitem v
        assert_equal 71 [r xlen mystream]

        set len_before_trim [r xlen mystream]
        set trimmed [r XTRIM mystream MAXLEN ~ 50 LIMIT 30]
        assert {$trimmed > 0 && $trimmed <= 30}
        assert_equal [expr {$len_before_trim - $trimmed}] [r xlen mystream]

        set fp [open $aof r]
        fconfigure $fp -translation binary
        set blob [read $fp]
        close $fp
        puts "AOF-PATH: $aof"
        puts "AOF-SIZE: [string length $blob]"
        puts "LIMIT-POS: [string first "LIMIT" $blob]"
        assert_equal -1 [string first "LIMIT" $blob]

        r debug loadaof
        assert_equal [expr {$len_before_trim - $trimmed}] [r xlen mystream]
    }
}
