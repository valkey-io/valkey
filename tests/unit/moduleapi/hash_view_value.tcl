set testmodule [file normalize tests/modules/hash_view_value.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module hash set} {
        r hash.set_view k f hello
        assert_equal "hello" [r hget k f]
        r hgetall k
        r hset k f hello1
        r hash.set_view k f hello2
        assert_equal "hello2" [r hget k f]
        r hdel k f
        r hash.set_view k f hello3
        assert_equal "hello3" [r hget k f]
    }

    test "Unload the module - hash" {
        assert_equal {OK} [r module unload hash.set_view]
    }
}
