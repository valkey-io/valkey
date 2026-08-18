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

    test "Replica should reject slot-management commands" {
        assert_error {ERR Please use ADDSLOTS/DELSLOTS only with primaries*} {R 2 CLUSTER ADDSLOTS 100}
        assert_error {ERR Please use ADDSLOTS/DELSLOTS only with primaries*} {R 3 CLUSTER DELSLOTS 100}
        assert_error {ERR Please use ADDSLOTSRANGE/DELSLOTSRANGE only with primaries*} {R 2 CLUSTER ADDSLOTSRANGE 100 200}
        assert_error {ERR Please use ADDSLOTSRANGE/DELSLOTSRANGE only with primaries*} {R 3 CLUSTER DELSLOTSRANGE 100 200}
        assert_error {ERR Please use FLUSHSLOTS only with primaries*} {R 2 CLUSTER FLUSHSLOTS}
    }

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

    # Regression tests for WATCHed keys that hash to a different slot than the transaction's commands. EXEC
    # committed such a transaction even when the WATCHed key had expired or had been removed.
    test {WATCHed key in another slot that expired aborts EXEC} {
        set watched_key "{tag1}watched"
        set transaction_key "{tag2}written-by-exec"
        assert {[R 0 cluster keyslot $watched_key] != [R 0 cluster keyslot $transaction_key]}

        # The TTL is set before WATCH, and the key is still in the keyspace once it elapses, so nothing but
        # the key having expired can abort the transaction.
        R 0 debug set-active-expire 0
        R 0 del $transaction_key
        R 0 set $watched_key alive px 50
        R 0 watch $watched_key
        set keys_before [R 0 dbsize]
        after 100
        assert_equal $keys_before [R 0 dbsize]

        R 0 multi
        R 0 set $transaction_key committed
        set reply [R 0 exec]
        R 0 debug set-active-expire 1

        assert_equal {} $reply
        assert_equal 0 [R 0 exists $transaction_key]
    }

    test {WATCHed key in another slot that is untouched commits EXEC} {
        set watched_key "{tag1}watched-alive"
        set transaction_key "{tag2}written-by-exec"
        assert {[R 0 cluster keyslot $watched_key] != [R 0 cluster keyslot $transaction_key]}

        R 0 set $watched_key alive
        R 0 watch $watched_key

        R 0 multi
        R 0 set $transaction_key committed
        assert_equal {OK} [R 0 exec]
        assert_equal {committed} [R 0 get $transaction_key]
    }

    test {WATCHed key in another slot flushed by a transaction aborts the watcher's EXEC} {
        set watched_key "{tag1}watched-flush"
        set transaction_key "{tag2}written-with-flush"
        assert {[R 0 cluster keyslot $watched_key] != [R 0 cluster keyslot $transaction_key]}

        set watcher [valkey_client 0]
        $watcher set $watched_key alive
        $watcher watch $watched_key

        R 0 multi
        R 0 set $transaction_key committed
        R 0 flushall
        assert_equal {OK OK} [R 0 exec]

        $watcher multi
        $watcher set $transaction_key committed-by-watcher
        set reply [$watcher exec]
        $watcher close

        assert_equal {} $reply
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
        assert_equal "ok" [getInfoProperty [R 0 cluster info] cluster_config_save_status]
        assert_equal "ok" [getInfoProperty [R 1 cluster info] cluster_config_save_status]

        # cluster_config_last_save_time should be set to a non-zero unix time on startup.
        set last_save_time_0 [getInfoProperty [R 0 cluster info] cluster_config_last_save_time]
        set last_save_time_1 [getInfoProperty [R 1 cluster info] cluster_config_last_save_time]
        assert_morethan $last_save_time_0 0
        assert_morethan $last_save_time_1 0

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
        R 0 debug bio-drain BIO_CLUSTER_SAVE
        R 1 debug bio-drain BIO_CLUSTER_SAVE
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
        R 0 debug bio-drain BIO_CLUSTER_SAVE
        R 1 debug bio-drain BIO_CLUSTER_SAVE
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

        # cluster_config_last_save_time must have advanced after a successful save.
        assert_morethan_equal [getInfoProperty [R 0 cluster info] cluster_config_last_save_time] $last_save_time_0
        assert_morethan_equal [getInfoProperty [R 1 cluster info] cluster_config_last_save_time] $last_save_time_1
    }
}

# Test that changing cluster-require-full-coverage triggers an immediate
# cluster state update. Use 3 primaries with no replicas so that pausing
# one primary leaves a slot range uncovered without automatic failover.
start_cluster 3 0 {tags {external:skip cluster} overrides {cluster-require-full-coverage yes}} {
    test "Cluster state transitions when toggling cluster-require-full-coverage" {
        # Pause one primary so part of the slots is uncovered.
        pause_process [srv 0 pid]
        wait_for_cluster_state fail

        # With cluster-require-full-coverage set to no, the R1 should
        # stay in the OK state even though some slots are uncovered.
        R 1 config set cluster-require-full-coverage no
        assert_equal [CI 1 cluster_state] "ok"
        assert_equal [CI 2 cluster_state] "fail"

        # Now change cluster-require-full-coverage back to yes. The R1
        # should immediately transition to FAIL because slots are uncovered.
        # Without the fix, the cluster would remain stuck in OK because
        # clusterUpdateState was only called when cluster is FAIL in clusterCron.
        R 1 config set cluster-require-full-coverage yes
        assert_equal [CI 1 cluster_state] "fail"
        assert_equal [CI 2 cluster_state] "fail"

        # Resume the paused primary to bring the cluster back to OK.
        resume_process [srv 0 pid]
        wait_for_cluster_state ok
    }
}
