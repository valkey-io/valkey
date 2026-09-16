start_cluster 1 1 {tags {path-hash external:skip cluster}} {
    test {Path Hash commands use the top-level key slot and replicate in cluster mode} {
        set key {path-hash:{model-a}:placement}
        assert_equal OK [R 0 phset $key a fields 1 worker-a generation-1]
        assert_equal OK [R 0 phset $key ab fields 1 worker-a generation-1]
        assert_equal {1 2} [R 0 phprefixes $key abc lengths]
        set primary [srv 0 client]
        set replica [srv -1 client]
        wait_for_ofs_sync $primary $replica
        $replica readonly
        assert_equal [list generation-1] [$replica phget $key ab worker-a]
        assert_equal 2 [$replica phcard $key]
    }
}
