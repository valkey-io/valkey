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

start_cluster 2 0 {tags {cluster external:skip needs:debug}} {
    test "A lost PING does not leave a node stuck in PFAIL when the peer keeps sending PINGs" {
        set CLUSTER_PACKET_TYPE_MEET 2
        set CLUSTER_PACKET_TYPE_NONE -1
        set CLUSTER_PACKET_TYPE_ALL -2
        set R1_nodeid [R 1 cluster myid]

        # Configure different timeout values to better reproduce the issue.
        R 0 config set cluster-node-timeout 15000
        R 1 config set cluster-node-timeout 1500

        # Drop all packets on R0 and wait for pfail.
        R 0 debug drop-cluster-packet-filter $CLUSTER_PACKET_TYPE_ALL
        wait_for_condition 1000 50 {
           [cluster_has_flag [cluster_get_node_by_id 0 $R1_nodeid] "fail?"]
        } else {
            puts "R 0 cluster nodes:"
            puts [R 0 cluster nodes]
            fail "R0 did not mark R1 as PFAIL"
        }

        # Remember some information after the PFAIL.
        set R0_ping_sent [dict get [cluster_get_node_by_id 0 $R1_nodeid] ping_sent]
        set R0_ping_received [CI 0 cluster_stats_messages_ping_received]

        # Restore the DEBUG setting on R0, but exclude MEET first to avoid
        # multiple MEET reconnections. We will restore it after R0 receives
        # the PING and refreshes data_received.
        R 0 debug drop-cluster-packet-filter $CLUSTER_PACKET_TYPE_MEET
        wait_for_condition 1000 50 {
           [CI 0 cluster_stats_messages_ping_received] > $R0_ping_received
        } else {
            fail "R0 did not receive the PING"
        }
        R 0 debug drop-cluster-packet-filter $CLUSTER_PACKET_TYPE_NONE

        # Ensure ping_sent does not get stuck and R0 can send PINGs.
        wait_for_condition 1000 50 {
           [dict get [cluster_get_node_by_id 0 $R1_nodeid] ping_sent] != $R0_ping_sent
        } else {
            puts "R 0 cluster nodes:"
            puts [R 0 cluster nodes]
            fail "R0 did not send the PING"
        }

        # All packets are being sent and received normally, and R0 should be
        # able to remove the PFAIL flag.
        wait_for_condition 1000 50 {
            ![cluster_has_flag [cluster_get_node_by_id 0 $R1_nodeid] "fail?"]
        } else {
            puts "R 0 cluster nodes:"
            puts [R 0 cluster nodes]
            fail "R0 did not remove the PFAIL flag"
        }
        wait_for_cluster_state ok
    }
}
