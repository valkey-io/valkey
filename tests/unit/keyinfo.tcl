start_server {tags {"keyinfo"} overrides {keyinfo-num-elements-larger-than 2 keyinfo-large-num-elements-max-len 128}} {
    test {KEYINFO - check that it starts with an empty log} {
        r keyinfo reset many-elements
        assert_equal [r keyinfo len many-elements] 0
    }

    test {KEYINFO - The ID for the same key must remain consistent} {
        r keyinfo reset many-elements

        r hset key-id0 f1 v1  f2 v2 f3 v3
        set e [lindex [r keyinfo get -1 many-elements] 0]
        assert_equal [expr {[lindex $e 0] == 89}] 1
        r hset key-id0 f4 v4
        set e [lindex [r keyinfo get -1 many-elements] 0]
        assert_equal [expr {[lindex $e 0] == 89}] 1

        r set key-id1 12345
        set e [lindex [r keyinfo get -1 many-elements] 1]
        assert_equal [expr {[lindex $e 0] == 120}] 1
        r set key-id1 123456
        set e [lindex [r keyinfo get -1 many-elements] 1]z
        assert_equal [expr {[lindex $e 0] == 120}] 1
    }

    test {KEYINFO - If the string length exceeds keyinfo-num-elements-larger-than, it must be recorded in keyinfo} {
        r keyinfo reset many-elements
        
        r set key-string 1
        assert_equal [r keyinfo len many-elements] 0
        r set key-string 12
        assert_equal [r keyinfo len many-elements] 0
        r set key-string 123
        assert_equal [r keyinfo len many-elements] 1

        set e [lindex [r keyinfo get -1 many-elements] 0]
        assert_equal [llength $e] 4
        assert_equal [lindex $e 1] {key-string}
        assert_equal [expr {[lindex $e 2] == 3}] 1

        r del key-string
        assert_equal [r keyinfo len many-elements] 0
    }

    test {KEYINFO - If the number of elements in a hash exceeds keyinfo-num-elements-larger-than, it must be recorded in keyinfo} {
        r keyinfo reset many-elements
        
        r hset key-hash f1 v1
        assert_equal [r keyinfo len many-elements] 0
        r hset key-hash f2 v2
        assert_equal [r keyinfo len many-elements] 0
        r hset key-hash f3 v3
        assert_equal [r keyinfo len many-elements] 1

        set e [lindex [r keyinfo get -1 many-elements] 0]
        assert_equal [llength $e] 4
        assert_equal [lindex $e 1] {key-hash}
        assert_equal [expr {[lindex $e 2] == 3}] 1

        r hdel key-hash f3
        assert_equal [r keyinfo len many-elements] 0
    }

    test {KEYINFO - If the number of elements in a list exceeds keyinfo-num-elements-larger-than, it must be recorded in keyinfo} {
        r keyinfo reset many-elements
        
        r lpush key-list m1
        assert_equal [r keyinfo len many-elements] 0
        r lpush key-list m2
        assert_equal [r keyinfo len many-elements] 0
        r lpush key-list m3
        assert_equal [r keyinfo len many-elements] 1

        set e [lindex [r keyinfo get -1 many-elements] 0]
        assert_equal [llength $e] 4
        assert_equal [lindex $e 1] {key-list}
        assert_equal [expr {[lindex $e 2] == 3}] 1

        r lpop key-list
        assert_equal [r keyinfo len many-elements] 0
    }

    test {KEYINFO - If the number of elements in a set exceeds keyinfo-num-elements-larger-than, it must be recorded in keyinfo} {
        r keyinfo reset many-elements
        
        r sadd key-set m1
        assert_equal [r keyinfo len many-elements] 0
        r sadd key-set m2
        assert_equal [r keyinfo len many-elements] 0
        r sadd key-set m3
        assert_equal [r keyinfo len many-elements] 1

        set e [lindex [r keyinfo get -1 many-elements] 0]
        assert_equal [llength $e] 4
        assert_equal [lindex $e 1] {key-set}
        assert_equal [expr {[lindex $e 2] == 3}] 1

        r srem key-set m3
        assert_equal [r keyinfo len many-elements] 0
    }

    test {KEYINFO - If the number of elements in a zset exceeds keyinfo-num-elements-larger-than, it must be recorded in keyinfo} {
        r keyinfo reset many-elements
        
        r zadd key-zset 1 m1
        assert_equal [r keyinfo len many-elements] 0
        r zadd key-zset 2 m2
        assert_equal [r keyinfo len many-elements] 0
        r zadd key-zset 3 m3
        assert_equal [r keyinfo len many-elements] 1

        set e [lindex [r keyinfo get -1 many-elements] 0]
        assert_equal [llength $e] 4
        assert_equal [lindex $e 1] {key-zset}
        assert_equal [expr {[lindex $e 2] == 3}] 1

        r zrem key-zset m3
        assert_equal [r keyinfo len many-elements] 0
    }
}
