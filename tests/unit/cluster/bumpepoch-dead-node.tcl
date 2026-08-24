test "CLUSTER BUMPEPOCH is not stuck on a tie with a dead node" {
    start_cluster 1 0 {tags {external:skip cluster}} {
        set R0_nodeid [R 0 CLUSTER MYID]
        set R0_epoch [CI 0 cluster_my_epoch]
        set dead_nodeid "0000000000000000000000000000000000000000"

        # Shutdown R0.
        set conf_path [cluster_nodes_conf_path 0]
        catch {R 0 shutdown nosave}

        # Modify R0's nodes.conf to inject a dead node that holds the same
        # configEpoch as R0
        set content [read_file $conf_path]
        set lines [split $content "\n"]
        set new_lines {}
        foreach line $lines {
            if {[string first $R0_nodeid $line] == 0} {
                # We first write the original R0 line, then modify it and
                # write it as the new dead node line.
                lappend new_lines $line
                regsub $R0_nodeid $line $dead_nodeid line
                regsub {myself} $line {fail,noaddr} line
                lappend new_lines $line
            }
        }
        set new_content [join $new_lines "\n"]
        write_file $conf_path $new_content

        # Restart the R0, R0 and the dead node have the same epoch.
        restart_server 0 true false
        set R0_epoch [dict get [cluster_get_node_by_id 0 $R0_nodeid] config_epoch]
        set dead_epoch [dict get [cluster_get_node_by_id 0 $dead_nodeid] config_epoch]
        assert_equal $R0_epoch $dead_epoch

        # Ensure the bump is successful even if they have the same epoch.
        assert_equal [R 0 cluster bumpepoch] "BUMPED [expr $R0_epoch + 1]"
    }
}

test "CLUSTER BUMPEPOCH jumps above a dead node with a very large configEpoch" {
    start_cluster 1 0 {tags {external:skip cluster}} {
        set R0_nodeid [R 0 CLUSTER MYID]
        set R0_epoch [CI 0 cluster_my_epoch]
        set dead_nodeid "0000000000000000000000000000000000000000"
        set dead_epoch 999999
        assert_morethan $dead_epoch $R0_epoch

        # Shutdown R0.
        set conf_path [cluster_nodes_conf_path 0]
        catch {R 0 shutdown nosave}

        # Modify R0's nodes.conf to inject a dead node with a very large
        # configEpoch.
        set content [read_file $conf_path]
        set lines [split $content "\n"]
        set new_lines {}
        foreach line $lines {
            if {[string first $R0_nodeid $line] == 0} {
                # We first write the original R0 line, then modify it and
                # write it as the new dead node line.
                lappend new_lines $line
                regsub $R0_nodeid $line $dead_nodeid line
                regsub {myself} $line {fail,noaddr} line
                regsub "$R0_epoch connected" $line "$dead_epoch connected" line
                lappend new_lines $line
            }
        }
        set new_content [join $new_lines "\n"]
        write_file $conf_path $new_content

        # Restart the R0
        restart_server 0 true false

        # When loading nodes.conf, currentEpoch is automatically set to the
        # maximum epoch, so the bump will always succeed.
        set current_epoch [CI 0 cluster_current_epoch]
        set dead_epoch [dict get [cluster_get_node_by_id 0 $dead_nodeid] config_epoch]
        assert_equal $dead_epoch $current_epoch
        assert_equal [R 0 cluster bumpepoch] "BUMPED [expr $dead_epoch + 1]"
    }
}
