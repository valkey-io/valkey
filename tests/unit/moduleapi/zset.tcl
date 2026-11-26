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

    foreach {enc max_entries} {listpack 128 skiplist 0} {
        test "Module key handle closes cleanly after a Call turns its string into a $enc zset" {
            r config set zset-max-listpack-entries $max_entries
            r del k src
            # A RAW string makes dbSetValue swap the zset into the robj the
            # handle still points at. An EMBSTR would be replaced instead.
            r set k [string repeat x 300]
            assert_equal raw [r object encoding k]
            r zadd src 1 a 2 b 3 c
            assert_equal $enc [r object encoding src]
            # Key type 1 is STRING, 5 is ZSET.
            assert_equal {1 5} [r zset.close_after_call k src]
            assert_equal {a b c} [r zrange k 0 -1]
            assert_equal PONG [r ping]
        }
    }
    r config set zset-max-listpack-entries 128

    test "Unload the module - zset" {
        assert_equal {OK} [r module unload zset]
    }
}
