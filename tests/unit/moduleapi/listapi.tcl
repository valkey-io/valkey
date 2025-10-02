set testmodule [file normalize tests/modules/test_module_listapi.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module queue add, size, range} {
        r listapi.add 1
        r listapi.add 2
        r listapi.add 3
        r listapi.add 4
        r listapi.add 5

        assert_equal 5 [r listapi.size]
        assert_equal {1 2 3} [r listapi.range 3]
        assert_equal {1 2 3 4 5} [r listapi.range 7]

        assert_equal {OK} [r listapi.reset]
        assert_equal 0 [r listapi.size]
    }

    test "Unload the module - list" {
        assert_equal {OK} [r module unload listapi]
    }
}
