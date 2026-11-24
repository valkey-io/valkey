test "Recover primary role from noflags created via replica ref after nodes.conf edit" {
    start_cluster 3 3 {tags {external:skip cluster}} {
        # R0 is a primary.
        set R0_nodeid [R 0 CLUSTER MYID]

        # Shutdown R5.
        set conf_path [cluster_nodes_conf_path 5]
        catch {R 5 shutdown nosave}

        # Modify R5's nodes.conf to remove the line for R0.
        set content [read_file $conf_path]
        set lines [split $content "\n"]
        set new_lines {}
        foreach line $lines {
            if {[string first $R0_nodeid $line] != 0} {
                lappend new_lines $line
            }
        }
        set new_content [join $new_lines "\n"]
        write_file $conf_path $new_content

        # Restart the R5 and ensure the flags eventually become consistent.
        # It loads nodes.conf with R0 missing, so R0 is created as noflags
        # when R0's own replica entry references it.
        restart_server -5 true false
        wait_for_condition 1000 50 {
            [cluster_has_flag [cluster_get_node_by_id 5 $R0_nodeid] master] eq 1
        } else {
            puts "R 5 cluster nodes:"
            puts [R 5 cluster nodes]
            fail "The node is not marked with the correct flag"
        }
    }
}

test "Recover primary and replica roles from noflags after nodes.conf edit" {
    start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-ping-interval 100 cluster-node-timeout 3000}} {
        # R0 is a primary, and R4 is a replica, they are not in the same shard.
        set R0_nodeid [R 0 CLUSTER MYID]
        set R4_nodeid [R 4 CLUSTER MYID]

        # Shutdown R5.
        set conf_path [cluster_nodes_conf_path 5]
        catch {R 5 shutdown nosave}

        # Modify the nodes.conf file of R5, setting R0 and R4 to noflags.
        set content [read_file $conf_path]
        set lines [split $content "\n"]
        set new_lines {}
        foreach line $lines {
            if {[string first $R0_nodeid $line] == 0} {
                regsub {master} $line {noflags} line
            }
            if {[string first $R4_nodeid $line] == 0} {
                regsub {slave} $line {noflags} line
            }
            lappend new_lines $line
        }
        set new_content [join $new_lines "\n"]
        write_file $conf_path $new_content

        # Restart the R5 and ensure the flags eventually become consistent.
        restart_server -5 true false
        wait_for_condition 1000 50 {
            [cluster_has_flag [cluster_get_node_by_id 5 $R0_nodeid] master] eq 1 &&
            [cluster_has_flag [cluster_get_node_by_id 5 $R4_nodeid] slave] eq 1
        } else {
            puts "R 5 cluster nodes:"
            puts [R 5 cluster nodes]
            fail "The node is not marked with the correct flag"
        }
    }
}

test "Recover replica role from noflags with residual slots after nodes.conf edit" {
    start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-ping-interval 100 cluster-node-timeout 3000}} {
        # R4 is a replica of R1 (a different shard than R5's primary R2).
        set R4_nodeid [R 4 CLUSTER MYID]

        # Shutdown R5.
        set conf_path [cluster_nodes_conf_path 5]
        catch {R 5 shutdown nosave}

        # Set R4 to noflags with residual slots 0-100 and a high configEpoch.
        # Prepend its line so it claims the slots before the real primary.
        set content [read_file $conf_path]
        set R4_line {}
        set new_lines {}
        foreach line [split $content "\n"] {
            if {[string first $R4_nodeid $line] == 0} {
                regsub {slave} $line {noflags} line
                regsub {(\d+) connected} $line {99999 connected} line
                set R4_line "$line 0-100"
            } else {
                lappend new_lines $line
            }
        }
        set new_lines [linsert $new_lines 0 $R4_line]
        set new_content [join $new_lines "\n"]
        write_file $conf_path $new_content

        # Restart the R5 and ensure the flags eventually become consistent
        # and the residual slots are cleared.
        restart_server -5 true false
        wait_for_condition 1000 50 {
            [cluster_has_flag [cluster_get_node_by_id 5 $R4_nodeid] slave] eq 1 &&
            [dict get [cluster_get_node_by_id 5 $R4_nodeid] slots] eq {}
        } else {
            puts "R 5 cluster nodes:"
            puts [R 5 cluster nodes]
            fail "The recovered replica still owns residual slots"
        }
    }
}
