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

proc file_inode {path} {
    file stat $path st
    return $st(ino)
}

# Make sure the test infra won't use SELECT.
set old_singledb $::singledb
set ::singledb 1

start_server {tags {external:skip cluster} overrides {cluster-enabled yes}} {
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
            assert_match "*already used by a different Redis Cluster node*" [exec cat $logfile]
        }
    }
}

set ::singledb $old_singledb

start_cluster 2 0 {tags {tls:skip external:skip cluster regression} overrides {cluster-allow-replica-migration no cluster-node-timeout 1000} } {
    # Issue #563 regression test
    test "Client blocked on XREADGROUP while stream's slot is migrated" {
        set stream_name aga
        set slot 609

        # Start a deferring client to simulate a blocked client on XREADGROUP
        R 0 XGROUP CREATE $stream_name mygroup $ MKSTREAM
        set rd [redis_deferring_client]
        $rd xreadgroup GROUP mygroup consumer BLOCK 0 streams $stream_name >
        wait_for_blocked_client

        # Migrate the slot to the target node
        R 0 CLUSTER SETSLOT $slot MIGRATING [dict get [cluster_get_myself 1] id]
        R 1 CLUSTER SETSLOT $slot IMPORTING [dict get [cluster_get_myself 0] id]

        # This line should cause the crash
        R 0 MIGRATE 127.0.0.1 [lindex [R 1 CONFIG GET port] 1] $stream_name 0 5000
    }
}
