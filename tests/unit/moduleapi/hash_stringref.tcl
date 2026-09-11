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

    test {Module hash has_stringref on non-existent field} {
        r del k
        r hset k f hello1
        # Setting a stringref converts the hash to the hashtable encoding, which
        # is the only encoding that looks the field up in the hashtable.
        r hash.set_stringref k f hello1
        assert_encoding hashtable k
        # Missing field must return 0 rather than dereferencing a NULL entry.
        assert_equal "0" [r hash.has_stringref k nonexistent]
    }

    test "Unload the module - hash" {
        assert_equal {OK} [r module unload hash.stringref]
    }
}
