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

start_cluster 2 0 {tags {external:skip cluster}} {
    # MSETEX key positions are counted pairs, so cluster routing needs msetexGetKeys.
    # Without it the command looks keyless and a non-owner executes the write.
    test {MSETEX cluster key routing} {
        set remote remotekey
        while {![catch {R 0 exists $remote}]} {
            append remote x
        }
        if {[catch {R 1 exists $remote} err]} {
            fail "node 1 should own $remote ($err)"
        }

        set remote_slot [R 0 cluster keyslot $remote]
        set remote_before [R 0 cluster countkeysinslot $remote_slot]
        assert_error {MOVED *} {R 0 msetex 1 $remote hello}
        assert_equal $remote_before [R 0 cluster countkeysinslot $remote_slot]
        assert_equal 1 [R 1 msetex 1 $remote hello]
        assert_equal hello [R 1 get $remote]
        assert_equal $remote_before [R 0 cluster countkeysinslot $remote_slot]
        R 1 del $remote

        set cross_a "{slotA}a"
        set cross_b "{slotB}b"
        assert {[R 0 cluster keyslot $cross_a] != [R 0 cluster keyslot $cross_b]}
        set slot_a [R 0 cluster keyslot $cross_a]
        set slot_b [R 0 cluster keyslot $cross_b]
        set before_a0 [R 0 cluster countkeysinslot $slot_a]
        set before_b0 [R 0 cluster countkeysinslot $slot_b]
        set before_a1 [R 1 cluster countkeysinslot $slot_a]
        set before_b1 [R 1 cluster countkeysinslot $slot_b]
        assert_error {CROSSSLOT Keys*} {R 0 msetex 2 $cross_a hello $cross_b world}
        assert_equal $before_a0 [R 0 cluster countkeysinslot $slot_a]
        assert_equal $before_b0 [R 0 cluster countkeysinslot $slot_b]
        assert_equal $before_a1 [R 1 cluster countkeysinslot $slot_a]
        assert_equal $before_b1 [R 1 cluster countkeysinslot $slot_b]

        set k1 "{msetex}a"
        set k2 "{msetex}b"
        assert {[R 0 cluster keyslot $k1] == [R 0 cluster keyslot $k2]}
        set owner 0
        if {[catch {R 0 exists $k1}]} {set owner 1}
        set other [expr {$owner ? 0 : 1}]
        set same_slot [R 0 cluster keyslot $k1]
        assert_equal 1 [R $owner msetex 2 $k1 hello $k2 world]
        assert_equal hello [R $owner get $k1]
        assert_equal world [R $owner get $k2]
        assert_equal 0 [R $other cluster countkeysinslot $same_slot]

        assert_equal [list $k1 $k2] [R $owner command getkeys msetex 2 $k1 v1 $k2 v2 ex 10]
        assert_error {MOVED *} {R $other msetex 1 $k1 hello EX 60}
        assert_equal hello [R $owner get $k1]
        assert_equal 1 [R $owner msetex 1 $k1 hello2 EX 60]
        assert_morethan [R $owner pttl $k1] 0

        # 257 keys is past the inline getKeys buffer (MAX_KEYS_BUFFER 256).
        set args [list 257]
        for {set i 0} {$i < 257} {incr i} {
            lappend args "{msetex}k$i" "v$i"
        }
        assert_equal 1 [R $owner msetex {*}$args]
        assert_equal v0 [R $owner get "{msetex}k0"]
        assert_equal v256 [R $owner get "{msetex}k256"]
        assert_equal 0 [R $other cluster countkeysinslot $same_slot]

        R 0 multi
        assert_error {MOVED *} {R 0 msetex 1 $remote gone}
        assert_error {EXECABORT*} {R 0 exec}
        assert_equal 0 [R 0 cluster countkeysinslot $remote_slot]
        assert_equal 0 [R 1 exists $remote]

        R 0 multi
        assert_error {CROSSSLOT Keys*} {R 0 msetex 2 $cross_a hello $cross_b world}
        assert_error {EXECABORT*} {R 0 exec}

        R $owner multi
        assert_equal QUEUED [R $owner msetex 2 $k1 q1 $k2 q2]
        assert_equal {1} [R $owner exec]
        assert_equal q1 [R $owner get $k1]
        assert_equal q2 [R $owner get $k2]

        set delargs [list $k1 $k2]
        for {set i 0} {$i < 257} {incr i} {
            lappend delargs "{msetex}k$i"
        }
        R $owner del {*}$delargs
    }
}

# Create a folder called "nodes.conf" to trigger temp nodes.conf rename
# failure and it will cause cluster config file save to fail at the rename.
proc create_nodes_conf_folder {srv_idx} {
    set dir [lindex [R $srv_idx config get dir] 1]
    set cluster_conf [lindex [R $srv_idx config get cluster-config-file] 1]
    set cluster_conf_path [file join $dir $cluster_conf]
    if {[file exists $cluster_conf_path]} { exec rm -f $cluster_conf_path }
    exec mkdir -p $cluster_conf_path
}

proc file_inode {path} {
    file stat $path st
    return $st(ino)
}

# The temp files a save leaves behind, if any. A save writes one and renames it
# over the cluster config file, so none should survive a completed save.
proc leftover_nodes_conf_tmp_files {srv_idx} {
    set dir [lindex [R $srv_idx config get dir] 1]
    set cluster_conf [lindex [R $srv_idx config get cluster-config-file] 1]
    set cluster_conf_path [file join $dir $cluster_conf]
    return [glob -nocomplain "${cluster_conf_path}.tmp-*"]
}

# Wait until node srv_idx has no temp files left. Cluster bus messages can
# start a save at any time, and that save's temp file exists until its rename
# fails and it deletes the file. So check again a few times. A temp file that
# is never deleted still fails.
proc wait_for_no_nodes_conf_tmp_files {srv_idx} {
    wait_for_condition 10 100 {
        [leftover_nodes_conf_tmp_files $srv_idx] eq {}
    } else {
        fail "Node $srv_idx left temp files: [leftover_nodes_conf_tmp_files $srv_idx]"
    }
}

start_cluster 1 1 {tags {external:skip cluster} overrides {cluster-config-save-behavior sync}} {
    test {cluster-config-save-behavior sync mode - node exits when config save fails} {
        # Create folder that can cause the rename fail.
        create_nodes_conf_folder 1

        # Trigger a takeover so that cluster will need to update the config file.
        catch {R 1 cluster failover takeover}

        # Wait for R1 to exit due to config save failure.
        wait_for_condition 1000 50 {
            [process_is_alive [srv -1 pid]] == 0
        } else {
            fail "R1 did not exit"
        }

        # Verify that save failure and fatal exit logs were printed.
        verify_log_message -1 "*Could not rename tmp cluster config file*" 0
        verify_log_message -1 "*Fatal: can't update cluster config file*" 0
    }
}

start_cluster 1 1 {tags {external:skip cluster} overrides {cluster-config-save-behavior best-effort}} {
    test {cluster-config-save-behavior best-effort mode - node continues running when config save fails} {
        # Create folder that can cause the rename fail.
        create_nodes_conf_folder 0
        create_nodes_conf_folder 1

        # Trigger a takeover so that cluster will need to update the config file.
        R 1 cluster failover takeover
        wait_for_condition 1000 50 {
            [s 0 role] eq {slave} &&
            [s -1 role] eq {master}
        } else {
            fail "The failover does not happen"
        }

        # Make sure the process is still alive, we won't exit when fail to save the config file.
        assert_equal {PONG} [R 0 ping]
        assert_equal {PONG} [R 1 ping]
        assert_equal 1 [process_is_alive [srv 0 pid]]
        assert_equal 1 [process_is_alive [srv -1 pid]]

        # Make sure relevant logs are printed.
        verify_log_message 0 "*Could not rename tmp cluster config file*" 0
        verify_log_message -1 "*Could not rename tmp cluster config file*" 0
        verify_log_message 0 "*Cluster config updated even though writing the cluster config file to disk failed*" 0
        verify_log_message -1 "*Cluster config updated even though writing the cluster config file to disk failed*" 0

        # Trigger a takeover so that cluster will need to update the config file.
        # We will not frequently print the "save failed" log.
        R 0 cluster failover takeover
        wait_for_condition 1000 50 {
            [s 0 role] eq {master} &&
            [s -1 role] eq {slave}
        } else {
            fail "The failover does not happen"
        }
        assert_morethan_equal [count_log_message 0 "Could not rename tmp cluster config file"] 2
        assert_equal [count_log_message 0 "Cluster config updated even though writing the cluster config file to disk failed"] 1
        assert_morethan_equal [count_log_message -1 "Could not rename tmp cluster config file"] 2
        assert_equal [count_log_message -1 "Cluster config updated even though writing the cluster config file to disk failed"] 1

        # A node whose renames keep failing must not accumulate temp files. This
        # holds before this commit too; it is here to keep the invariant from
        # regressing as the save path is reordered.
        wait_for_no_nodes_conf_tmp_files 0
        wait_for_no_nodes_conf_tmp_files 1
    }
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
            set pid [exec $::VALKEY_SERVER_BIN --port 0 \
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
