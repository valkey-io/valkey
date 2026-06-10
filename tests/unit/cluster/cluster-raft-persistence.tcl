# Test raft cluster persistence (nodes.conf format, recovery, corruption).

proc is_alive {pid} {
    if {[catch {exec kill -0 $pid}]} {
        return 0
    }
    return 1
}

tags {external:skip cluster singledb} {

test "Raft persistence: corrupt log line CRC is discarded on load" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
        # Wait for the singleton to be ready.
        R 0 CLUSTER ADDSLOTSRANGE 0 16383
        wait_for_condition 50 100 {
            [CI 0 cluster_state] eq "ok"
        } else {
            fail "Cluster never reached ok state"
        }

        # Get nodes.conf path and server pid.
        set dir [lindex [R 0 CONFIG GET dir] 1]
        set nodes_conf "$dir/nodes.conf"
        set pid [srv 0 pid]

        # Kill with SIGKILL (no shutdown handler, no rewrite).
        exec kill -9 $pid
        wait_for_condition 50 100 {
            ![is_alive $pid]
        } else {
            fail "Server didn't die"
        }

        # Append a corrupt log line (bad CRC) with a fake NODE_JOIN.
        set fake_id [string repeat f 40]
        set fake_addr "127.0.0.1:9999@19999,,tls-port=0,shard-id=[string repeat a 40]"
        set corrupt_line "log 0000000000000000 99 1 NODE_JOIN $fake_id $fake_addr"
        set fp [open $nodes_conf a]
        puts $fp $corrupt_line
        close $fp

        # Restart server in the same directory.
        restart_server 0 true true

        # The corrupt NODE_JOIN should be discarded — only myself known.
        assert_equal 1 [CI 0 cluster_known_nodes]

        # Verify the corruption warning was logged.
        verify_log_message 0 "*Corrupt log line*CRC mismatch*" 0
    }
}

} ;# tags
