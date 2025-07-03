set testmodule [file normalize tests/modules/hash_externalizer.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module hash set} {
        r hash.extern k f hello
        assert_equal "hello" [r hget k f]
        r hgetall k
    }

    test "Unload the module - hash" {
        assert_equal {OK} [r module unload hash_externalizer]
    }
}
