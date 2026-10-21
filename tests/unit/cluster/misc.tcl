start_cluster 2 2 {tags {external:skip cluster}} {
    test {Key lazy expires during key migration} {
        R 0 DEBUG SET-ACTIVE-EXPIRE 0

        set key_slot [R 0 CLUSTER KEYSLOT FOO]
        R 0 set FOO BAR PX 10
        set src_id [R 0 CLUSTER MYID]
        set trg_id [R 1 CLUSTER MYID]
        R 0 CLUSTER SETSLOT $key_slot MIGRATING $trg_id
        R 1 CLUSTER SETSLOT $key_slot IMPORTING $src_id
        after 11
        assert_error {ASK*} {R 0 GET FOO}
        R 0 ping
    } {PONG}

    test "Coverage: Basic cluster commands" {
        assert_equal {OK} [R 0 CLUSTER saveconfig]

        set id [R 0 CLUSTER MYID]
        assert_equal {0} [R 0 CLUSTER count-failure-reports $id]

        R 0 flushall
        assert_equal {OK} [R 0 CLUSTER flushslots]
    }
}

start_cluster 1 1 {tags {external:skip cluster}} {
    test {Cross-slot transaction} {
        assert_equal OK [R 0 multi]
        assert_equal QUEUED [r get foo]
        assert_equal QUEUED [r get bar]
        assert_error {CROSSSLOT *} {r exec}
    }
}

proc file_inode {path} {
    file stat $path st
    return $st(ino)
}

start_server {tags {external:skip cluster singledb} overrides {cluster-enabled yes}} {
    test {nodes.conf stays locked after the config is saved} {
        set dir [lindex [r config get dir] 1]
        set cluster_conf [lindex [r config get cluster-config-file] 1]
        set conf_path [file join $dir $cluster_conf]

        # Every save publishes nodes.conf with rename(), which puts a new inode
        # under the name. Force one, then confirm the running node still holds
        # the lock that stops a second node from using the same file.
        set inode_before [file_inode $conf_path]
        assert_equal {OK} [r cluster saveconfig]
        wait_for_condition 50 100 {
            [file_inode $conf_path] != $inode_before
        } else {
            fail "the save did not replace $cluster_conf"
        }

        # flock() does not exist on Solaris, so clusterLockConfig() does not lock
        # at all there and a second node is expected to start.
        if {$::tcl_platform(os) ne "SunOS"} {
            set logfile [file join $dir second-node.log]
            set pid [exec src/valkey-server --port 0 \
                         --unixsocket [file join $dir second-node.sock] \
                         --cluster-enabled yes \
                         --dir $dir \
                         --cluster-config-file $cluster_conf \
                         --logfile $logfile &]

            wait_for_condition 100 100 {
                ![process_is_alive $pid]
            } else {
                catch {exec kill -9 $pid}
                fail "a second node started while $cluster_conf was already in use"
            }
            assert_match "*already used by a different Cluster node*" [exec cat $logfile]
        }
    }
}
