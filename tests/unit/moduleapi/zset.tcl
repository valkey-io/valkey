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

        test "Module zset range iteration steps to the neighbour in both directions - $enc" {
            r del k lex
            r zadd k 1 a 2 b 3 c 4 d 5 e
            r zadd lex 0 a 0 b 0 c 0 d 0 e
            assert_equal $enc [r object encoding k]
            # Walk the range to its end and back to its start.
            assert_equal {b c d END c b END} [r zset.walk k score 2 4 nnnppp]
            assert_equal {b c d END c b END} [r zset.walk lex lex \[b \[d nnnppp]
            # Reversing direction must not revisit the current element.
            assert_equal {b c b c b} [r zset.walk k score 2 4 npnp]
            assert_equal {b c b c b} [r zset.walk lex lex \[b \[d npnp]
            # Stepping back from the first element ends the iteration and
            # leaves the cursor usable.
            assert_equal {b END c d} [r zset.walk k score 2 4 pnn]
            assert_equal {b END c d} [r zset.walk lex lex \[b \[d pnn]
            # Walking backwards from the last element of the range.
            assert_equal {d c b END} [r zset.walk k score-last 2 4 ppp]
            assert_equal {d c b END} [r zset.walk lex lex-last \[b \[d ppp]
            assert_equal {d END c} [r zset.walk k score-last 2 4 np]
            # Same, with the range reaching both ends of the key.
            assert_equal {a b c d e END d c b a END b} [r zset.walk k score -inf +inf nnnnnpppppn]
            assert_equal {a b c d e END d c b a END b} [r zset.walk lex lex - + nnnnnpppppn]
        }

        test "Module zset range end-reached flag follows the last step - $enc" {
            r del k
            r zadd k 1 a 2 b 3 c 4 d 5 e
            assert_equal $enc [r object encoding k]
            assert_equal {b 0 c d END 1 c 0 b END 1 c 0} [r zset.walk k score 2 4 ennnepeppene]
            assert_equal {{} 1 END 1} [r zset.walk k score 10 20 ene]
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

    test "Module zset write through the open key ends the range iteration - listpack" {
        r del k
        r zadd k 1 a 2 b 3 c 4 d 5 e
        assert_equal listpack [r object encoding k]
        # Every write reallocates or shifts the listpack the iterator points
        # into. The iteration must end instead of following the old pointer.
        assert_equal {1 {} 0} [r zset.writewhilewalking k first rem a b c]
        assert_equal {1 {} 0} [r zset.writewhilewalking k last add x y z]
        assert_equal {1 {} 0} [r zset.writewhilewalking k first incrby d e]
        assert_equal {x 0 y 0 z 0 d 5 e 6} [r zrange k 0 -1 withscores]
    }

    r config set zset-max-listpack-entries 0
    set members {}
    for {set i 0} {$i < 2000} {incr i} {
        lappend members $i [format m%04d $i]
    }

    test "Module zset write through the open key ends the range iteration - btree, emptied leaf" {
        r del k
        r zadd k {*}$members
        assert_equal btree [r object encoding k]
        # Removing the first 100 members empties the leaf that holds the
        # iterator's current element, so the leaf is freed.
        set first100 {}
        for {set i 0} {$i < 100} {incr i} {
            lappend first100 [format m%04d $i]
        }
        assert_equal {1 {} 0} [r zset.writewhilewalking k first rem {*}$first100]
        assert_equal 1900 [r zcard k]
        assert_equal m0100 [lindex [r zrange k 0 0] 0]
    }

    test "Module zset write through the open key ends the range iteration - btree, compacted leaves" {
        r del k
        r zadd k {*}$members
        assert_equal btree [r object encoding k]
        # Removing two of every three members leaves the leaves sparse without
        # emptying any of them, so the delete path compacts them and frees the
        # surplus leaves, among them the one holding the last element m1999,
        # which is never removed itself.
        set scattered {}
        for {set i 1} {$i < 1998} {incr i} {
            if {$i % 3 != 0} {
                lappend scattered [format m%04d $i]
            }
        }
        assert_equal {1 {} 0} [r zset.writewhilewalking k last rem {*}$scattered]
        assert_equal [expr {2000 - [llength $scattered]}] [r zcard k]
        assert_equal m1999 [lindex [r zrange k -1 -1] 0]
    }
    r config set zset-max-listpack-entries 128

    test "Unload the module - zset" {
        assert_equal {OK} [r module unload zset]
    }
}
