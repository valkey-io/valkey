set testmodule [file normalize tests/modules/stringops.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {CreateStringUninitialized works} {
        r del k
        assert_equal {OK} [r stringops.create_uninit_string_set k "hello world"]
        assert_equal "hello world" [r get k]

        # With more challenging string
        assert_equal {OK} [r stringops.create_uninit_string_set k "a\x00b\x00c"]
        assert_equal "a\x00b\x00c" [r get k]

        # With zero-length string
        assert_equal {OK} [r stringops.create_uninit_string_set k ""]
        assert_equal "" [r get k]
    }

    test {StringSetMove moves value into key} {
        r del k
        assert_equal {OK} [r stringops.setmove k "moved value"]
        assert_equal "moved value" [r get k]
    }

    test {StringSetMove does not double-free the auto-memory string} {
        # If the moved string has bad refcount math, the auto-memory collector
        # frees a value the DB owns.
        # Loop and read back so the failure trips valgrind in CI.
        for {set i 0} {$i < 200} {incr i} {
            r stringops.setmove k "value-$i"
            assert_equal "value-$i" [r get k]
        }
    }

    test {StringSetMove refuses a shared (refcount > 1) string} {
        r del k
        assert_equal {REFUSED} [r stringops.setmove_fail k]
        # Key must be untouched by the refused move.
        assert_equal 0 [r exists k]
    }

    test {StringSetMove does not bypass key opened as READ} {
        r set k "set value"
        assert_error "REFUSED" {r stringops.setmove_on_readonly_key k "will not set"}
        # Key must not be created by READ access
        assert_equal "set value" [r get k]
    }

    test {StringSetMove un-sets ttl} {
        r set k "set value" ex 10
        assert_equal 10 [r ttl k]
        assert_equal "OK" [r stringops.setmove k "new value"]
        assert_equal "new value" [r get k]
        assert_equal -1 [r ttl k]
    }

    test {CreateStringReferenceFromKey returns the string value} {
        r set k "reference me"
        assert_equal "reference me" [r stringops.getref k]
    }

    test {CreateStringReferenceFromKey returns null for a missing key} {
        r del k
        assert_equal {} [r stringops.getref k]
    }

    test {CreateStringReferenceFromKey returns null for a non-string key} {
        r del k
        r rpush k a b c
        assert_equal {} [r stringops.getref k]
    }

    test {Reference survives deletion of the originating key} {
        r set k "still in use"
        assert_equal {OK} [r stringops.ref_capture k]
        r del k
        assert_equal "still in use" [r stringops.ref_read]
        assert_equal {OK} [r stringops.ref_release]
    }

    test {Reference survives overwrite of the originating key} {
        r set k "still here"
        assert_equal {OK} [r stringops.ref_capture k]
        r set k "and then i showed up"
        assert_equal "still here" [r stringops.ref_read]
        assert_equal "and then i showed up" [r get k]
        assert_equal {OK} [r stringops.ref_release]
    }

    test "Unload the module - stringops" {
        assert_equal {OK} [r module unload stringops]
    }
}
