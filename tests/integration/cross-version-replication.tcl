# Test replication from an older version primary.
#
# Use minimal.conf to make sure we don't use any configs not supported on the old version.
start_server {tags {"repl needs:other-server external:skip compatible-redis"} start-other-server 1 config "minimal.conf"} {
    set hello [r hello]
    set primary_name_and_version "[dict get $hello server] [dict get $hello version]"
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

# Test replication from the current version to an older version replica.
start_server {tags {"repl needs:other-server external:skip"}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    $primary config set repl-diskless-sync yes
    $primary config set repl-diskless-sync-delay 1

    # As a side-effect, this first start_server block initializes old_replica_version which
    # is used in the tests below.
    start_server {start-other-server 1 config "minimal.conf"} {
        set hello [r hello]
        set old_replica_version [dict get $hello version]
        # set replica_name_and_version "[dict get $hello server] $replica_version"
        set old_replica [srv 0 client]
        start_server {} {
            set new_replica [srv 0 client]
            test {Keys can be sync'ed by old and new replicas} {
                $primary set foo bar
                $old_replica replicaof $primary_host $primary_port
                $new_replica replicaof $primary_host $primary_port
                wait_for_sync $old_replica 500 100
                wait_for_sync $new_replica 500 100
                assert_equal bar [$old_replica get foo]
                assert_equal bar [$new_replica get foo]
            }
        }
    }

    test "Old pre-HFE replica can't sync but doesn't prevent new replica from sync" {
        if {[version_greater_or_equal $old_replica_version 9.0.0]} {
            skip "Replica $old_replica_version does support HFE"
        }
        r flushall
        r hsetex hfe ex 1000 fields 1 field1 value1
        start_server {start-other-server 1 config "minimal.conf"} {
            set old_replica [srv 0 client]
            start_server {} {
                set new_replica [srv 0 client]
                $old_replica replicaof $primary_host $primary_port
                $new_replica replicaof $primary_host $primary_port
                wait_for_sync $new_replica 500 100
                wait_for_log_messages -2 [list {*Can't store key 'hfe'*}] 0 50 100
                assert_equal value1 [$new_replica hget hfe field1]
                assert_match {*master_link_status:up*} [$new_replica info replication]
                assert_match {*master_link_status:down*} [$old_replica info replication]
            }
        }
    }

    test "Replica with HFE support can full sync" {
        if {![version_greater_or_equal $old_replica_version 9.0.0]} {
            skip "Replica $old_replica_version doesn't support HFE"
        }
        r flushall
        r hsetex hfe ex 1000 fields 1 field1 value1
        start_server {start-other-server 1 config "minimal.conf"} {
            set old_replica [srv 0 client]
            $old_replica replicaof $primary_host $primary_port
            wait_for_sync $old_replica 500 100
            assert_equal value1 [$old_replica hget hfe field1]
        }
    }
}
