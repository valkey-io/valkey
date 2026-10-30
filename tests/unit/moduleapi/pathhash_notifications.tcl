set testmodule [file normalize tests/modules/pathhash_notifications.so]
set miscmodule [file normalize tests/modules/misc.so]

tags {modules pathhash external:skip} {
    start_server [list overrides [list loadmodule $testmodule appendonly yes aof-use-rdb-preamble no]] {
        test {Path Hash module notifications observe completed deletions} {
            foreach protocol {2 3} {
                r hello $protocol
                foreach deletion {
                    {phdel ph-notify p f}
                    {phdel ph-notify p}
                    {phdelprefix ph-notify p}
                    {phdelprefix ph-notify {}}
                } {
                    r phset ph-notify p fields 1 f v
                    r pathhash.notify.configure observe
                    assert_equal 1 [r {*}$deletion]
                    assert_equal {1 1 0} [r pathhash.notify.stats]
                    assert_equal 0 [r {*}$deletion]
                    assert_equal {1 1 0} [r pathhash.notify.stats]
                }
                r phset ph-notify p fields 2 f v g w
                r pathhash.notify.configure observe
                assert_equal 1 [r phdel ph-notify p f]
                assert_equal {1 0 1} [r pathhash.notify.stats]
                assert_equal 1 [r phdel ph-notify p g]
                assert_equal {2 1 0} [r pathhash.notify.stats]
            }
            if {$::force_resp3} {r hello 3} else {r hello 2}
        }

        start_server {} {
            set primary [srv -1 client]
            set replica [srv 0 client]
            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_condition 50 100 {[s 0 master_link_status] eq {up}} else {
                fail "Replication not started"
            }

            test {Path Hash post-notification writes follow deletion in replication and AOF} {
                foreach deletion {
                    {phdel ph-notify p f}
                    {phdel ph-notify p}
                    {phdelprefix ph-notify p}
                    {phdelprefix ph-notify {}}
                } {
                    $primary pathhash.notify.configure observe
                    $primary del ph-notify
                    $primary phset ph-notify p fields 1 f original
                    $primary pathhash.notify.configure recreate
                    assert_equal 1 [$primary {*}$deletion]
                    assert_equal {1 1 0} [$primary pathhash.notify.stats]
                    assert_equal {restored} [$primary phget ph-notify p f]
                    wait_for_ofs_sync $primary $replica
                    assert_equal {restored} [$replica phget ph-notify p f]
                    $primary debug loadaof
                    assert_equal {restored} [$primary phget ph-notify p f]
                }
                $primary bgrewriteaof
                waitForBgrewriteaof $primary
                $primary debug loadaof
                assert_equal {restored} [$primary phget ph-notify p f]
            } {} {needs:debug}

            test {Path Hash deletion uses the module caller propagation flags} {
                $primary pathhash.notify.configure observe
                $primary module load $miscmodule
                foreach flag {!A !R} {
                    set key ph-notify-$flag
                    $primary phset $key p fields 1 f original
                    wait_for_ofs_sync $primary $replica
                    assert_equal 1 [$primary test.rm_call_flags $flag phdel $key p]
                    assert_equal 0 [$primary exists $key]
                    wait_for_ofs_sync $primary $replica
                    assert_equal [expr {$flag eq "!R"}] [$replica exists $key]
                }
                $primary debug loadaof
                assert_equal 1 [$primary exists ph-notify-!A]
                assert_equal 0 [$primary exists ph-notify-!R]
            } {} {needs:debug}
        }
    }
}
