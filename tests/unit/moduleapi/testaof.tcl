# testaof module configuration flags:
# 1 << 0 = 1 - aux before keyspace
# 1 << 1 = 2 - aux after keyspace

set testmodule [file normalize tests/modules/testaof.so]

tags "modules" {

    # 3 == 11 - before + after keyspace
    test {AOF module aux: persist and restore globals before and after keyspace} {
        set server_path [tmpdir "server.module-testaof-both"]
        start_server [list overrides [list loadmodule "$testmodule 3" "dir" $server_path appendonly yes aof-use-rdb-preamble no save ""] keep_persistence true] {
            r testaof.set.before aof_before_val
            r testaof.set.after aof_after_val
            assert_equal "aof_before_val" [r testaof.get.before]
            assert_equal "aof_after_val" [r testaof.get.after]
            r bgrewriteaof
            waitForBgrewriteaof r
        }
        start_server [list overrides [list loadmodule "$testmodule 3" "dir" $server_path appendonly yes aof-use-rdb-preamble no save ""]] {
            assert_equal "aof_before_val" [r testaof.get.before]
            assert_equal "aof_after_val" [r testaof.get.after]
        }
    }

    # 2 == 10 - after keyspace only
    test {AOF module aux: persist and restore globals after keyspace only} {
        set server_path [tmpdir "server.module-testaof-after"]
        start_server [list overrides [list loadmodule "$testmodule 2" "dir" $server_path appendonly yes aof-use-rdb-preamble no save ""] keep_persistence true] {
            r testaof.set.after aof_after_only
            assert_equal "aof_after_only" [r testaof.get.after]
            r bgrewriteaof
            waitForBgrewriteaof r
        }
        start_server [list overrides [list loadmodule "$testmodule 2" "dir" $server_path appendonly yes aof-use-rdb-preamble no save ""]] {
            assert_equal "aof_after_only" [r testaof.get.after]
        }
    }

    # 3 == 11 - before + after, but no data set
    test {AOF module aux: no MODULE AUXLOAD written when no data is set} {
        set server_path [tmpdir "server.module-testaof-nodata"]
        start_server [list overrides [list loadmodule "$testmodule 3" "dir" $server_path appendonly yes aof-use-rdb-preamble no save ""] keep_persistence true] {
            r set mykey myvalue
            r bgrewriteaof
            waitForBgrewriteaof r
        }
        # Restart without the module - should succeed because no MODULE AUXLOAD was written
        start_server [list overrides [list "dir" $server_path appendonly yes aof-use-rdb-preamble no save ""]] {
            assert_equal "myvalue" [r get mykey]
        }
    }

    # 3 == 11 - before + after, coexists with regular keys
    test {AOF module aux: data coexists with regular keys} {
        set server_path [tmpdir "server.module-testaof-coexist"]
        start_server [list overrides [list loadmodule "$testmodule 3" "dir" $server_path appendonly yes aof-use-rdb-preamble no save ""] keep_persistence true] {
            r set normalkey normalvalue
            r set anotherkey anothervalue
            r testaof.set.before aux_before
            r testaof.set.after aux_after
            r bgrewriteaof
            waitForBgrewriteaof r
        }
        start_server [list overrides [list loadmodule "$testmodule 3" "dir" $server_path appendonly yes aof-use-rdb-preamble no save ""]] {
            assert_equal "normalvalue" [r get normalkey]
            assert_equal "anothervalue" [r get anotherkey]
            assert_equal "aux_before" [r testaof.get.before]
            assert_equal "aux_after" [r testaof.get.after]
        }
    }
}
