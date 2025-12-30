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

# Get the path to the cluster config file.
proc get_nodes_conf_path {srv_idx} {
    set dir [lindex [R $srv_idx config get dir] 1]
    set cluster_conf [lindex [R $srv_idx config get cluster-config-file] 1]
    set cluster_conf_path [file join $dir $cluster_conf]
    return $cluster_conf_path
}

# Create a folder called "nodes.conf" to trigger temp nodes.conf rename
# failure and it will cause cluster config file save to fail at the rename.
proc create_nodes_conf_folder {srv_idx} {
    set cluster_conf_path [get_nodes_conf_path $srv_idx]
    if {[file exists $cluster_conf_path]} { exec rm -rf $cluster_conf_path }
    exec mkdir -p $cluster_conf_path
}

# Remove the folder (or nodes.conf) that we created from create_nodes_conf_folder.
proc remove_nodes_conf_folder {srv_idx} {
    set cluster_conf_path [get_nodes_conf_path $srv_idx]
    exec rm -rf $cluster_conf_path
}

start_cluster 1 1 {tags {external:skip cluster}} {
    test {Fail to save the cluster configuration file will not exit the process} {
        assert_equal "ok" [getInfoProperty [R 0 cluster info] cluster_config_save_status]
        assert_equal "ok" [getInfoProperty [R 1 cluster info] cluster_config_save_status]

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

        # Check the info field is err.
        assert_equal "err" [getInfoProperty [R 0 cluster info] cluster_config_save_status]
        assert_equal "err" [getInfoProperty [R 1 cluster info] cluster_config_save_status]

        # Remove the test folder to trigger a config save again.
        remove_nodes_conf_folder 0
        remove_nodes_conf_folder 1

        # Trigger a takeover so that cluster will need to update the config file.
        R 1 cluster failover takeover
        wait_for_condition 1000 50 {
            [s 0 role] eq {slave} &&
            [s -1 role] eq {master}
        } else {
            fail "The failover does not happen"
        }

        # Check the info field is ok.
        wait_for_condition 1000 50 {
            [getInfoProperty [R 0 cluster info] cluster_config_save_status] eq "ok" &&
            [getInfoProperty [R 1 cluster info] cluster_config_save_status] eq "ok"
        } else {
            fail "The config save status is not ok"
        }

        # Create folder that can cause the rename fail.
        create_nodes_conf_folder 0
        create_nodes_conf_folder 1

        # saveconfig will fail and info field is err.
        assert_error {ERR *} {R 0 CLUSTER saveconfig}
        assert_error {ERR *} {R 1 CLUSTER saveconfig}
        assert_equal "err" [getInfoProperty [R 0 cluster info] cluster_config_save_status]
        assert_equal "err" [getInfoProperty [R 1 cluster info] cluster_config_save_status]

        # Remove the test folder to trigger a config save again.
        remove_nodes_conf_folder 0
        remove_nodes_conf_folder 1

        # saveconfig will success and info field is ok.
        assert_equal {OK} [R 0 CLUSTER saveconfig]
        assert_equal {OK} [R 1 CLUSTER saveconfig]
        assert_equal "ok" [getInfoProperty [R 0 cluster info] cluster_config_save_status]
        assert_equal "ok" [getInfoProperty [R 1 cluster info] cluster_config_save_status]
    }
}
