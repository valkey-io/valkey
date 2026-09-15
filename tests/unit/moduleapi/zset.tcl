set testmodule [file normalize tests/modules/zset.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module zset rem} {
        r del k
        r zadd k 100 hello 200 world
        assert_equal 1 [r zset.rem k hello]
        assert_equal 0 [r zset.rem k hello]
        assert_equal 1 [r exists k]
        # Check that removing the last element deletes the key
        assert_equal 1 [r zset.rem k world]
        assert_equal 0 [r exists k]
    }

    test {Module zset add} {
        r del k
        # Check that failure does not create empty key
        assert_error "ERR ZsetAdd failed" {r zset.add k nan hello}
        assert_equal 0 [r exists k]

        r zset.add k 100 hello
        assert_equal {hello 100} [r zrange k 0 -1 withscores]
    }

    test {Module zset incrby} {
        r del k
        # Check that failure does not create empty key
        assert_error "ERR ZsetIncrby failed" {r zset.incrby k hello nan}
        assert_equal 0 [r exists k]

        r zset.incrby k hello 100
        assert_equal {hello 100} [r zrange k 0 -1 withscores]
    }

    test {Module zset rangebylex} {
        # Should give wrong arity error
        assert_error "ERR wrong number of arguments*" {r zset.rangebylex}
        assert_error "ERR wrong number of arguments*" {r zset.revrangebylex}

        # Should give wrong type error
        r del k
        r set k v
        assert_error "WRONGTYPE Operation against a key*" {r zset.rangebylex k - +}

        # Should give invalid range error
        r del k
        r zadd k 0 ele
        assert_error "invalid range" {r zset.rangebylex k - a}
        assert_error "invalid range" {r zset.revrangebylex k - a}

        # Check if the data structure of the sorted set is skiplist
        r del k
        r config set zset-max-listpack-entries 2
        r config set zset-max-listpack-value 64
        for {set i 0} {$i < 4} {incr i} {
            r zadd k 0 "ele$i"
        }
        assert_equal {ele0 ele1 ele2 ele3} [r zset.rangebylex k - +]
        assert_equal {ele3 ele2 ele1 ele0} [r zset.revrangebylex k - +]
        assert_equal {ele1 ele2} [r zset.rangebylex k "(ele0" "(ele3"]
        assert_equal {ele2 ele1} [r zset.revrangebylex k "(ele0" "(ele3"]

        # Check if the data structure of the sorted set is listpack
        r del k
        r config set zset-max-listpack-entries 128
        r config set zset-max-listpack-value 64
        for {set i 0} {$i < 4} {incr i} {
            r zadd k 0 "ele$i"
        }
        assert_equal {ele0 ele1 ele2 ele3} [r zset.rangebylex k - +]
        assert_equal {ele3 ele2 ele1 ele0} [r zset.revrangebylex k - +]
        assert_equal {ele1 ele2} [r zset.rangebylex k "(ele0" "(ele3"]
        assert_equal {ele2 ele1} [r zset.revrangebylex k "(ele0" "(ele3"]
    }

    test {Module zset members} {
        # Should give wrong arity error
        assert_error "ERR wrong number of arguments*" {r zset.members}

        # Should give wrong type error
        r del k
        r set k v
        assert_error "WRONGTYPE Operation against a key*" {r zset.members k}

        # Check if the data structure of the sorted set is skiplist
        r del k
        r config set zset-max-listpack-entries 2
        r config set zset-max-listpack-value 64
        for {set i 0} {$i < 4} {incr i} {
            r zadd k 0 "ele$i"
        }
        assert_equal {ele0 ele1 ele2 ele3} [lsort [r zset.members k]]

        # Check if the data structure of the sorted set is listpack
        r del k
        r config set zset-max-listpack-entries 128
        r config set zset-max-listpack-value 64
        for {set i 0} {$i < 4} {incr i} {
            r zadd k 0 "ele$i"
        }
        assert_equal {ele0 ele1 ele2 ele3} [lsort [r zset.members k]]
    }

    foreach {enc max_entries} {listpack 128 btree 0} {
        r config set zset-max-listpack-entries $max_entries

        test "Module zset lex range with an empty or inverted range starts with no element - $enc" {
            r del lex
            r zadd lex 0 a 0 b 0 c 0 d 0 e
            assert_equal $enc [r object encoding lex]
            assert_equal {{} END} [r zset.walk lex lex \[c \[b n]
            assert_equal {{} END} [r zset.walk lex lex ( ( n]
            assert_equal {{} END} [r zset.walk lex lex-last (e + p]
            assert_equal {{} END} [r zset.walk lex lex-last \[f + p]
            assert_equal {} [r zset.rangebylex lex \[c \[b]
            assert_equal {} [r zset.revrangebylex lex (e +]
            # A non-empty range still starts on its first element.
            assert_equal {b c} [r zset.walk lex lex \[b \[d n]
            assert_equal {d c} [r zset.walk lex lex-last \[b \[d p]
            assert_error {ERR no such key} {r zset.walk nokey lex - + n}
        }

        test "Module zset score range with a NaN bound is empty - $enc" {
            r del k
            r zadd k 1 a 2 b
            assert_equal $enc [r object encoding k]
            assert_equal {{} END} [r zset.walk k score 0 nan n]
            assert_equal {{} END} [r zset.walk k score nan 5 n]
            assert_equal {{} END} [r zset.walk k score-last 0 nan p]
            assert_equal {{} END} [r zset.walk k score-last nan 5 p]
            assert_equal {a b} [r zset.walk k score 0 5 n]
        }
    }
    r config set zset-max-listpack-entries 128

    test "Unload the module - zset" {
        assert_equal {OK} [r module unload zset]
    }
}
