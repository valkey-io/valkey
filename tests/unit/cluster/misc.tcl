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
        assert_equal {} [leftover_nodes_conf_tmp_files 0]
        assert_equal {} [leftover_nodes_conf_tmp_files 1]
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
