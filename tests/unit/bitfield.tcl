start_server {tags {"bitops"}} {
    test {BITFIELD signed SET and GET basics} {
        r del bits
        set results {}
        lappend results [r bitfield bits set i8 0 -100]
        lappend results [r bitfield bits set i8 0 101]
        lappend results [r bitfield bits get i8 0]
        set results
    } {0 -100 101}

    test {BITFIELD unsigned SET and GET basics} {
        r del bits
        set results {}
        lappend results [r bitfield bits set u8 0 255]
        lappend results [r bitfield bits set u8 0 100]
        lappend results [r bitfield bits get u8 0]
        set results
    } {0 255 100}

    test {BITFIELD signed SET and GET together} {
        r del bits
        set results [r bitfield bits set i8 0 255 set i8 0 100 get i8 0]
    } {0 -1 100}
 
    test {BITFIELD unsigned with SET, GET and INCRBY arguments} {
        r del bits
        set results [r bitfield bits set u8 0 255 incrby u8 0 100 get u8 0]
    } {0 99 99}

    test {BITFIELD with only key as argument} {
        r del bits
        set result [r bitfield bits]
        assert {$result eq {}}
    }

    test {BITFIELD #<idx> form} {
        r del bits
        set results {}
        r bitfield bits set u8 #0 65
        r bitfield bits set u8 #1 66
        r bitfield bits set u8 #2 67
        r get bits
    } {ABC}

    test {BITFIELD basic INCRBY form} {
        r del bits
        set results {}
        r bitfield bits set u8 #0 10
        lappend results [r bitfield bits incrby u8 #0 100]
        lappend results [r bitfield bits incrby u8 #0 100]
        set results
    } {110 210}

    test {BITFIELD chaining of multiple commands} {
        r del bits
        set results {}
        r bitfield bits set u8 #0 10
        lappend results [r bitfield bits incrby u8 #0 100 incrby u8 #0 100]
        set results
    } {{110 210}}

    test {BITFIELD unsigned overflow wrap} {
        r del bits
        set results {}
        r bitfield bits set u8 #0 100
        lappend results [r bitfield bits overflow wrap incrby u8 #0 257]
        lappend results [r bitfield bits get u8 #0]
        lappend results [r bitfield bits overflow wrap incrby u8 #0 255]
        lappend results [r bitfield bits get u8 #0]
    } {101 101 100 100}

    test {BITFIELD unsigned overflow sat} {
        r del bits
        set results {}
        r bitfield bits set u8 #0 100
        lappend results [r bitfield bits overflow sat incrby u8 #0 257]
        lappend results [r bitfield bits get u8 #0]
        lappend results [r bitfield bits overflow sat incrby u8 #0 -255]
        lappend results [r bitfield bits get u8 #0]
    } {255 255 0 0}

    test {BITFIELD signed overflow wrap} {
        r del bits
        set results {}
        r bitfield bits set i8 #0 100
        lappend results [r bitfield bits overflow wrap incrby i8 #0 257]
        lappend results [r bitfield bits get i8 #0]
        lappend results [r bitfield bits overflow wrap incrby i8 #0 255]
        lappend results [r bitfield bits get i8 #0]
    } {101 101 100 100}

    test {BITFIELD signed overflow sat} {
        r del bits
        set results {}
        r bitfield bits set u8 #0 100
        lappend results [r bitfield bits overflow sat incrby i8 #0 257]
        lappend results [r bitfield bits get i8 #0]
        lappend results [r bitfield bits overflow sat incrby i8 #0 -255]
        lappend results [r bitfield bits get i8 #0]
    } {127 127 -128 -128}

    test {BITFIELD overflow detection fuzzing} {
        for {set j 0} {$j < 1000} {incr j} {
            set bits [expr {[randomInt 64]+1}]
            set sign [randomInt 2]
            set range [expr {2**$bits}]
            if {$bits == 64} {set sign 1} ; # u64 is not supported by BITFIELD.
            if {$sign} {
                set min [expr {-($range/2)}]
                set type "i$bits"
            } else {
                set min 0
                set type "u$bits"
            }
            set max [expr {$min+$range-1}]

            # Compare Tcl vs the server
            set range2 [expr {$range*2}]
            set value [expr {($min*2)+[randomInt $range2]}]
            set increment [expr {($min*2)+[randomInt $range2]}]
            if {$value > 9223372036854775807} {
                set value 9223372036854775807
            }
            if {$value < -9223372036854775808} {
                set value -9223372036854775808
            }
            if {$increment > 9223372036854775807} {
                set increment 9223372036854775807
            }
            if {$increment < -9223372036854775808} {
                set increment -9223372036854775808
            }

            set overflow 0
            if {$value > $max || $value < $min} {set overflow 1}
            if {($value + $increment) > $max} {set overflow 1}
            if {($value + $increment) < $min} {set overflow 1}

            r del bits
            set res1 [r bitfield bits overflow fail set $type 0 $value]
            set res2 [r bitfield bits overflow fail incrby $type 0 $increment]

            if {$overflow && [lindex $res1 0] ne {} &&
                             [lindex $res2 0] ne {}} {
                fail "OW not detected where needed: $type $value+$increment"
            }
            if {!$overflow && ([lindex $res1 0] eq {} ||
                               [lindex $res2 0] eq {})} {
                fail "OW detected where NOT needed: $type $value+$increment"
            }
        }
    }

    test {BITFIELD overflow wrap fuzzing} {
        for {set j 0} {$j < 1000} {incr j} {
            set bits [expr {[randomInt 64]+1}]
            set sign [randomInt 2]
            set range [expr {2**$bits}]
            if {$bits == 64} {set sign 1} ; # u64 is not supported by BITFIELD.
            if {$sign} {
                set min [expr {-($range/2)}]
                set type "i$bits"
            } else {
                set min 0
                set type "u$bits"
            }
            set max [expr {$min+$range-1}]

            # Compare Tcl vs the server
            set range2 [expr {$range*2}]
            set value [expr {($min*2)+[randomInt $range2]}]
            set increment [expr {($min*2)+[randomInt $range2]}]
            if {$value > 9223372036854775807} {
                set value 9223372036854775807
            }
            if {$value < -9223372036854775808} {
                set value -9223372036854775808
            }
            if {$increment > 9223372036854775807} {
                set increment 9223372036854775807
            }
            if {$increment < -9223372036854775808} {
                set increment -9223372036854775808
            }

            r del bits
            r bitfield bits overflow wrap set $type 0 $value
            r bitfield bits overflow wrap incrby $type 0 $increment
            set res [lindex [r bitfield bits get $type 0] 0]

            set expected 0
            if {$sign} {incr expected [expr {$max+1}]}
            incr expected $value
            incr expected $increment
            set expected [expr {$expected % $range}]
            if {$sign} {incr expected $min}

            if {$res != $expected} {
                fail "WRAP error: $type $value+$increment = $res, should be $expected"
            }
        }
    }

    test {BITFIELD regression for #3221} {
        r set bits 1
        r bitfield bits get u1 0
    } {0}

    test {BITFIELD regression for #3564} {
        for {set j 0} {$j < 10} {incr j} {
            r del mystring
            set res [r BITFIELD mystring SET i8 0 10 SET i8 64 10 INCRBY i8 10 99900]
            assert {$res eq {0 0 60}}
        }
        r del mystring
    }

    test {BITFIELD_RO with only key as argument} {
        set res [r bitfield_ro bits]
        assert {$res eq {}}
    }

    test {BITFIELD_RO fails when write option is used} {
        catch {r bitfield_ro bits set u8 0 100 get u8 0} err
        assert_match {*ERR BITFIELD_RO only supports the GET subcommand*} $err
    }
}

start_server {tags {"repl external:skip"}} {
    start_server {} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set slave [srv 0 client]

        test {BITFIELD: setup slave} {
            $slave slaveof $master_host $master_port
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }
        }

        test {BITFIELD: write on master, read on slave} {
            $master del bits
            assert_equal 0 [$master bitfield bits set u8 0 255]
            assert_equal 255 [$master bitfield bits set u8 0 100]
            wait_for_ofs_sync $master $slave
            assert_equal 100 [$slave bitfield_ro bits get u8 0]
        }

        test {BITFIELD_RO with only key as argument on read-only replica} {
            set res [$slave bitfield_ro bits]
            assert {$res eq {}}
        }

        test {BITFIELD_RO fails when write option is used on read-only replica} {
            catch {$slave bitfield_ro bits set u8 0 100 get u8 0} err
            assert_match {*ERR BITFIELD_RO only supports the GET subcommand*} $err
        }

        test {BITFIELD OVERFLOW FAIL on new key creates key on primary and is propagated} {
            # u8 max is 255; SET u8 300 overflows. With OVERFLOW FAIL the write is
            # skipped and nil is returned, but lookupStringForBitCommand has already
            # created the (zero-padded) key on the primary. That create is a
            # modification and must be propagated, or the replica never sees the key.
            assert_equal {{}} [$master bitfield k OVERFLOW FAIL SET u8 0 300]

            # Key exists on the master and is replicated.
            assert_equal 1 [$master exists k]
            wait_for_ofs_sync $master $slave
            assert_equal 1 [$slave exists k]
        }

        test {BITFIELD OVERFLOW FAIL that resizes an existing key is propagated to the replica} {
            # Pre-create a small (1-byte) string that is already replicated.
            $master setbit k2 0 1
            wait_for_ofs_sync $master $slave
            assert_equal 1 [$slave exists k2]
            assert_equal 1 [$slave strlen k2]

            # SET u8 at offset 100 grows the (zero-padded) value before the
            # overflow check runs. u8 max is 255; SET u8 300 overflows, so with
            # OVERFLOW FAIL the write is skipped and nil is returned -- but the
            # key has already been resized on the primary. The resize is itself a
            # modification: it must bump the dirty counter and be replicated, or
            # primary/replica sizes diverge.
            assert_equal {{}} [$master bitfield k2 OVERFLOW FAIL SET u8 100 300]

            set master_len [$master strlen k2]
            assert {$master_len > 1}

            # The resize must have been replicated, so the replica reports the
            # same (grown) length.
            wait_for_ofs_sync $master $slave
            assert_equal $master_len [$slave strlen k2]
        }
    }
}
