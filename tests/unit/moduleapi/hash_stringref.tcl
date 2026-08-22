set testmodule [file normalize tests/modules/hash_stringref.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module hash set} {
        r del k
        set status [catch {r hash.set_stringref k f hello} errmsg]
        assert {$status == 1}
        r hset k f hello1
        assert_equal "0" [r hash.has_stringref k f]
        r hash.set_stringref k f hello1
        assert_equal "hello1" [r hget k f]
        assert_equal "1" [r hash.has_stringref k f]
    }

    test {Module hash set_stringref on a field which does not exist} {
        r del k
        r hset k f hello1
        set status [catch {r hash.set_stringref k nofield hello2} errmsg]
        assert {$status == 1}
        assert_equal "hello1" [r hget k f]
        assert_equal "0" [r hash.has_stringref k f]
        r hash.set_stringref k f hello1
        assert_equal "hello1" [r hget k f]
        assert_equal "1" [r hash.has_stringref k f]
        assert_equal "0" [r hash.has_stringref k nofield]
    }

    test {Module hash has_stringref on a field which does not exist} {
        r del k
        r hset k f hello1
        assert_encoding listpack k
        assert_equal "0" [r hash.has_stringref k nofield]

        r hash.set_stringref k f hello1
        assert_encoding hashtable k
        assert_equal "1" [r hash.has_stringref k f]
        assert_equal "0" [r hash.has_stringref k nofield]
    }

    test {Module hash has_stringref on an expired field} {
        r del k
        r hset k f1 hello1 f2 hello2
        r hash.set_stringref k f1 hello1
        r hash.set_stringref k f2 hello2
        r hpexpire k 1 FIELDS 1 f1
        wait_for_condition 50 100 {
            [r hash.has_stringref k f1] eq 0
        } else {
            fail "Hash field did not expire"
        }
        assert_equal "1" [r hash.has_stringref k f2]
    }

    test {Module hash has_stringref on a missing key or a non hash key} {
        r del k
        assert_equal "0" [r hash.has_stringref k f]
        r lpush k v
        assert_equal "0" [r hash.has_stringref k f]
        r del k
    }

    test "Unload the module - hash" {
        assert_equal {OK} [r module unload hash.stringref]
    }
}
