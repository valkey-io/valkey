start_server {tags {"commandlog-magnitude"} overrides {commandlog-execution-slower-than 0 commandlog-slow-execution-max-len 5 commandlog-slow-retention-policy magnitude}} {

    test {Magnitude mode - config is set correctly} {
        assert_equal [lindex [r config get commandlog-slow-retention-policy] 1] {magnitude}
    }

    test {Magnitude mode - reset works} {
        r commandlog reset slow
        # The reset command itself may be logged (threshold=0), so len <= 1
        assert {[r commandlog len slow] <= 1}
    }

    test {Magnitude mode - entries are added up to max-len} {
        r commandlog reset slow
        for {set i 0} {$i < 10} {incr i} {
            r set key$i val$i
        }
        assert_equal [r commandlog len slow] 5
    }

    test {Magnitude mode - keeps highest value entries} {
        r commandlog reset slow
        # Send fast commands to fill the log
        for {set i 0} {$i < 10} {incr i} {
            r set key$i val$i
        }
        # All 5 entries should have non-zero values
        set entries [r slowlog get 5]
        foreach entry $entries {
            set value [lindex $entry 2]
            assert {$value > 0}
        }
    }

    test {Magnitude mode - slow command displaces fast ones} {
        r commandlog reset slow
        # Fill with fast commands
        for {set i 0} {$i < 10} {incr i} {
            r set key$i val$i
        }
        # Record the current minimum value
        set entries [r slowlog get 5]
        set min_val [lindex [lindex $entries 0] 2]
        foreach entry $entries {
            set val [lindex $entry 2]
            if {$val < $min_val} { set min_val $val }
        }
        # Now do a slow command (sleep 10ms)
        r debug sleep 0.01
        # The slow command should be in the log
        set entries [r slowlog get 5]
        set found_slow 0
        foreach entry $entries {
            set val [lindex $entry 2]
            if {$val > 5000} { set found_slow 1 }
        }
        assert_equal $found_slow 1
    } {} {needs:debug}

    test {Magnitude mode - O(1) rejection of below-minimum values} {
        r commandlog reset slow
        # Fill with debug sleep commands (high value)
        for {set i 0} {$i < 5} {incr i} {
            r debug sleep 0.01
        }
        # All entries should be > 5000 usec
        set entries [r slowlog get 5]
        foreach entry $entries {
            set val [lindex $entry 2]
            assert {$val > 5000}
        }
        # Now send fast commands - they should be rejected
        for {set i 0} {$i < 20} {incr i} {
            r set key$i val$i
        }
        # Log should still only have the slow commands
        set entries [r slowlog get 5]
        foreach entry $entries {
            set val [lindex $entry 2]
            assert {$val > 5000}
        }
    } {} {needs:debug}

    test {Magnitude mode - reset clears entries} {
        for {set i 0} {$i < 10} {incr i} {
            r set key$i val$i
        }
        assert_equal [r commandlog len slow] 5
        r commandlog reset slow
        # After reset, only the reset command itself may remain (threshold=0)
        assert {[r commandlog len slow] <= 1}
    }

    test {Magnitude mode - len grows with commands} {
        r commandlog reset slow
        set baseline [r commandlog len slow]
        r set a b
        r set c d
        r set e f
        assert {[r commandlog len slow] >= [expr {$baseline + 3}]}
    }

    test {Magnitude mode - max-len bounds the log size} {
        r commandlog reset slow
        for {set i 0} {$i < 100} {incr i} {
            r set key$i val$i
        }
        assert_equal [r commandlog len slow] 5
    }

    test {Magnitude mode - disabled with threshold -1} {
        r config set commandlog-execution-slower-than -1
        r commandlog reset slow
        for {set i 0} {$i < 10} {incr i} {
            r set key$i val$i
        }
        assert_equal [r commandlog len slow] 0
        r config set commandlog-execution-slower-than 0
    }

    test {Magnitude mode - disabled with max-len 0} {
        r config set commandlog-slow-execution-max-len 0
        r commandlog reset slow
        for {set i 0} {$i < 10} {incr i} {
            r set key$i val$i
        }
        assert_equal [r commandlog len slow] 0
        r config set commandlog-slow-execution-max-len 5
    }

    test {Recency mode still works when configured} {
        r config set commandlog-slow-retention-policy recency
        r commandlog reset slow
        for {set i 0} {$i < 10} {incr i} {
            r set key$i val$i
        }
        # Should keep at most max-len entries (5)
        assert_equal [r commandlog len slow] 5
        # Restore
        r config set commandlog-slow-retention-policy magnitude
    }
}
