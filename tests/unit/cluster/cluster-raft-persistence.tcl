# Test raft cluster persistence (nodes.conf format, recovery, corruption).

tags {external:skip cluster singledb} {

test "Raft persistence: corrupt log line CRC prevents startup" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
        # Get nodes.conf path and config file.
        set dir [lindex [R 0 CONFIG GET dir] 1]
        set nodes_conf "$dir/nodes.conf"
        set port [srv 0 port]
        set config_file [srv 0 config_file]

        # Stop server gracefully.
        catch {R 0 SHUTDOWN NOSAVE}

        # Append a corrupt log line (bad CRC) to the existing nodes.conf.
        set fake_id [string repeat f 40]
        set fake_addr "127.0.0.1:9999@19999,,tls-port=0,shard-id=[string repeat a 40]"
        set corrupt_line "log 0000000000000000 99 1 NODE_JOIN $fake_id $fake_addr"
        set fp [open $nodes_conf a]
        puts $fp $corrupt_line
        close $fp

        # Try to start server — it should panic.
        catch {exec src/valkey-server $config_file --port $port --dir $dir} output
        assert_match "*Corrupt raft log line: CRC mismatch*" $output

        # Restart cleanly for framework teardown (remove corrupt line).
        set fp [open $nodes_conf w]
        close $fp
        restart_server 0 true true
    }
}


test "Raft persistence: missing log entry (index gap) prevents startup" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
        set dir [lindex [R 0 CONFIG GET dir] 1]
        set nodes_conf "$dir/nodes.conf"
        set port [srv 0 port]
        set config_file [srv 0 config_file]

        # Stop server gracefully.
        catch {R 0 SHUTDOWN NOSAVE}

        # Append a valid log line at index 2 (skipping index 1) to create
        # a gap. CRC is precomputed for this exact payload.
        set line "log 33420fe746511517 2 1 NODE_JOIN ffffffffffffffffffffffffffffffffffffffff 127.0.0.1:9999@19999,,tls-port=0,shard-id=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        set fp [open $nodes_conf a]
        puts $fp $line
        close $fp

        # Try to start server — it should panic.
        catch {exec src/valkey-server $config_file --port $port --dir $dir} output
        assert_match "*Corrupt raft log: index gap*" $output

        # Restart cleanly for framework teardown (remove corrupt line).
        set fp [open $nodes_conf w]
        close $fp
        restart_server 0 true true
    }
}
} ;# tags
