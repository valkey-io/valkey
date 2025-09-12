# Test replication from an older version primary.
#
# Use minimal.conf to make sure we don't use any configs not supported on the old version.

proc server_name_and_version {} {
    set server_name [s server_name]
    if {$server_name eq {}} {
        set server_name redis
    }
    set server_version [s "${server_name}_version"]
    return "$server_name $server_version"
}

start_server {tags {"repl needs:other-server external:skip compatible-redis"} start-other-server 1 config "minimal.conf"} {
    set primary_name_and_version [server_name_and_version]
    r set foo bar

    start_server {} {
        test "Start replication from $primary_name_and_version" {
            r replicaof [srv -1 host] [srv -1 port]
            wait_for_sync r 500 100
            # The key has been transferred.
            assert_equal bar [r get foo]
            assert_equal up [s master_link_status]
        }

        test "Replicate a SET command from $primary_name_and_version" {
            r -1 set baz quux
            wait_for_ofs_sync [srv 0 client] [srv -1 client]
            set reply [r get baz]
            assert_equal $reply quux
        }
    }
}

proc log_file_matches {log pattern} {
    set fp [open $log r]
    set content [read $fp]
    close $fp
    string match $pattern $content
}

start_server {tags {"repl external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_log [srv 0 stdout]
    
    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]
        
        test "Replica data gets flushed during successful full sync" {
            # Force full sync 
            $master config set repl-backlog-size 1
            
            # Populate replica with initial data
            $replica set existing_key "original_value"
            assert_equal [$replica dbsize] 1
            
            # Populate master with different data
            $master set master_key "master_value"
            
            # Start replication - should succeed and flush replica data
            $replica replicaof $master_host $master_port
            
            # Wait for successful sync
            wait_for_condition 50 100 {
                [log_file_matches $replica_log "*PRIMARY <-> REPLICA sync: RDB compatability check passed. Flushing old data*"]
            } else {
                puts "Replica log contents:"
                puts [exec cat $replica_log]
                fail "Expected successful RDB sync not detected"
            }
            
            # Verify replica data was flushed and replaced with master data
            assert_equal [$replica get existing_key] ""
            assert_equal [$replica get master_key] "master_value"
            assert_equal [$replica dbsize] 1
        }
    }
}