start_cluster 1 1 {tags {radix external:skip cluster}} {
    test {Radix commands use the top-level key slot and replicate in cluster mode} {
        set key {radix:{model-a}:placement}
        assert_equal OK [R 0 rset $key a worker-a generation-1]
        assert_equal OK [R 0 rset $key ab worker-a generation-1]
        assert_equal {1 2} [R 0 rprefixes $key abc lengths]
        set primary [srv 0 client]
        set replica [srv -1 client]
        wait_for_ofs_sync $primary $replica
        $replica readonly
        assert_equal generation-1 [$replica rget $key ab worker-a]
        assert_equal 2 [$replica rcard $key]
    }
}
