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

    test "XACKDEL replicates as equivalent pre-9.2 commands XACK/XDEL for backwards compatibility" {
        if {[version_greater_or_equal $old_replica_version 9.2.0]} {
            skip "Replica $old_replica_version must be before 9.2.0 for this test"
        }

        r FLUSHALL
        r XADD mystream 1-0 hello world
        r XGROUP CREATE mystream grp1 0
        r XGROUP CREATE mystream grp2 0
        r XREADGROUP GROUP grp1 alice COUNT 1 STREAMS mystream >
        r XREADGROUP GROUP grp2 bob COUNT 1 STREAMS mystream >
        r XACKDEL mystream grp1 DELREF IDS 1 1-0
        start_server {start-other-server 1 config "minimal.conf"} {
            set old_replica [srv 0 client]
            $old_replica replicaof $primary_host $primary_port
            wait_for_sync $old_replica 500 100
            assert_equal [llength [$old_replica XRANGE mystream - +]] 0
            assert_equal [llength [$old_replica XPENDING mystream grp1 - + 10]] 0
            assert_equal [llength [$old_replica XPENDING mystream grp2 - + 10]] 0
        }
    }

    test "XDELEX replicates as equivalent pre-9.2 commands XACK/XDEL for backwards compatibility" {
        if {[version_greater_or_equal $old_replica_version 9.2.0]} {
            skip "Replica $old_replica_version must be before 9.2.0 for this test"
        }

        r FLUSHALL
        r XADD mystream 1-0 hello world
        r XGROUP CREATE mystream grp1 0
        r XGROUP CREATE mystream grp2 0
        r XREADGROUP GROUP grp1 alice COUNT 1 STREAMS mystream >
        r XREADGROUP GROUP grp2 bob COUNT 1 STREAMS mystream >
        r XDELEX mystream DELREF IDS 1 1-0
        start_server {start-other-server 1 config "minimal.conf"} {
            set old_replica [srv 0 client]
            $old_replica replicaof $primary_host $primary_port
            wait_for_sync $old_replica 500 100
            assert_equal [llength [$old_replica XRANGE mystream - +]] 0
            assert_equal [llength [$old_replica XPENDING mystream grp1 - + 10]] 0
            assert_equal [llength [$old_replica XPENDING mystream grp2 - + 10]] 0
        }
    }

    test "Old pre-Path Hash replica can't sync but doesn't prevent new replica from sync" {
        if {[version_greater_or_equal $old_replica_version 9.2.0]} {
            skip "Replica $old_replica_version does support Path Hash"
        }
        r flushall
        r phset pathhash path fields 1 field value
        start_server {start-other-server 1 config "minimal.conf"} {
            set old_replica [srv 0 client]
            start_server {} {
                set new_replica [srv 0 client]
                $old_replica replicaof $primary_host $primary_port
                $new_replica replicaof $primary_host $primary_port
                wait_for_sync $new_replica 500 100
                wait_for_log_messages -2 [list {*Can't store key 'pathhash'*}] 0 50 100
                assert_equal [list value] [$new_replica phget pathhash path field]
                assert_match {*master_link_status:up*} [$new_replica info replication]
                assert_match {*master_link_status:down*} [$old_replica info replication]
            }
        }
    }

    test "Replica with Path Hash support can full sync" {
        if {![version_greater_or_equal $old_replica_version 9.2.0]} {
            skip "Replica $old_replica_version doesn't support Path Hash"
        }
        r flushall
        r phset pathhash path fields 1 field value
        start_server {start-other-server 1 config "minimal.conf"} {
            set old_replica [srv 0 client]
            $old_replica replicaof $primary_host $primary_port
            wait_for_sync $old_replica 500 100
            assert_equal [list value] [$old_replica phget pathhash path field]
        }
    }
}

# Listpacks store, after each element, the length of that element, so that the
# structure can be traversed backwards. Three element lengths are written with
# two different widths depending on the version that wrote them, and a reader
# assuming the wrong width walks into the middle of the next element. Cover both
# directions against an old version, since a released reader cannot be fixed
# retroactively: whatever we write has to stay readable by versions already out.
#
# A 32-bit string element occupies 5 + len bytes, so a 16378-byte value gives an
# element length of exactly 16383, the first of the three. The others need a 2MB
# and a 256MB value, too slow to cover on every run.
set backlen_boundary_value [string repeat x 16378]

# wait_for_sync, but reporting a replica that died instead of letting the
# dropped connection surface as an I/O error. A replica that cannot parse the
# payload terminates, so that is the expected shape of a regression here.
proc wait_for_cross_version_sync {client descr} {
    for {set i 0} {$i < 300} {incr i} {
        if {[catch {set link [status $client master_link_status]}]} {
            fail "$descr: replica terminated while syncing, see its log for an RDB error"
        }
        if {$link eq "up"} return
        after 100
    }
    fail "$descr: replica did not sync in time (link status '$link')"
}

proc assert_backlen_stream_intact {client value descr} {
    # Reading the entries traverses the listpack across the boundary element.
    set entries [$client xrange backlen - +]
    assert_equal 2 [llength $entries] $descr
    assert_equal $value [dict get [lindex $entries 0 1] f] $descr
    assert_equal tail [dict get [lindex $entries 1 1] f] $descr
}

start_server {tags {"repl needs:other-server external:skip"}} {
    set new_primary [srv 0 client]
    set new_primary_host [srv 0 host]
    set new_primary_port [srv 0 port]
    $new_primary config set repl-diskless-sync yes
    $new_primary config set repl-diskless-sync-delay 0
    $new_primary xadd backlen 1-1 f $backlen_boundary_value
    $new_primary xadd backlen 1-2 f tail

    start_server {start-other-server 1 config "minimal.conf"} {
        set old_replica [srv 0 client]
        set old_version [dict get [$old_replica hello] version]

        test "Listpack boundary backlen written by current version is read by $old_version" {
            $old_replica replicaof $new_primary_host $new_primary_port
            wait_for_cross_version_sync $old_replica "current -> $old_version"
            assert_backlen_stream_intact $old_replica $backlen_boundary_value "current -> $old_version"
        }
    }
}

start_server {tags {"repl needs:other-server external:skip"} start-other-server 1 config "minimal.conf"} {
    set old_primary [srv 0 client]
    set old_primary_host [srv 0 host]
    set old_primary_port [srv 0 port]
    set old_version [dict get [$old_primary hello] version]
    $old_primary xadd backlen 1-1 f $backlen_boundary_value
    $old_primary xadd backlen 1-2 f tail

    start_server {} {
        set new_replica [srv 0 client]

        test "Listpack boundary backlen written by $old_version is read by current version" {
            $new_replica replicaof $old_primary_host $old_primary_port
            wait_for_cross_version_sync $new_replica "$old_version -> current"
            assert_backlen_stream_intact $new_replica $backlen_boundary_value "$old_version -> current"
        }

        test "Listpack boundary backlen from $old_version survives reload by current version" {
            # Re-encodes the listpack with our own writer, so this also covers
            # the width we produce being readable by ourselves.
            $new_replica debug reload
            assert_backlen_stream_intact $new_replica $backlen_boundary_value "reload"
        }
    }
}
