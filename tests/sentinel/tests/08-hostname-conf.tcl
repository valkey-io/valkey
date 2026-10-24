source "../tests/includes/utils.tcl"

proc set_redis_announce_ip {addr} {
    foreach_valkey_id id {
        R $id config set replica-announce-ip $addr
    }
}

proc set_sentinel_config {keyword value} {
    foreach_sentinel_id id {
        S $id sentinel config set $keyword $value
    }
}

proc set_all_instances_hostname {hostname} {
    foreach_sentinel_id id {
        set_instance_attrib sentinel $id host $hostname
    }
    foreach_valkey_id id {
        set_instance_attrib valkey $id host $hostname
    }
}

test "(pre-init) Configure instances and sentinel for hostname use" {
    set ::host "localhost"
    restart_killed_instances
    set_all_instances_hostname $::host
    set_redis_announce_ip $::host
    set_sentinel_config resolve-hostnames yes
    set_sentinel_config announce-hostnames yes
}

source "../tests/includes/init-tests.tcl"

proc verify_hostname_announced {hostname} {
    foreach_sentinel_id id {
        # Master is reported with its hostname
        if {![string equal [lindex [S $id SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster] 0] $hostname]} {
            return 0
        }

        # Replicas are reported with their hostnames
        foreach replica [S $id SENTINEL REPLICAS mymaster] {
            if {![string equal [dict get $replica ip] $hostname]} {
                return 0
            }
        }
    }
    return 1
}

test "Sentinel announces hostnames" {
    # Check initial state
    verify_hostname_announced $::host

    # Disable announce-hostnames and confirm IPs are used
    set_sentinel_config announce-hostnames no
    assert {[verify_hostname_announced "127.0.0.1"] || [verify_hostname_announced "::1"]}
}

test "Sentinel preserves unresolved replica hostnames across config rewrites" {
    S 0 SENTINEL CONFIG SET announce-hostnames yes
    kill_instance sentinel 0
    set f [open "sentinel_0/sentinel.conf" a]
    puts $f "sentinel monitor unresolved-primary primary.invalid 6379 1"
    puts $f "sentinel known-replica unresolved-primary replica-a.invalid 6379"
    puts $f "sentinel known-replica unresolved-primary replica-b.invalid 6379"
    close $f
    restart_instance sentinel 0
    test_defer {
        S 0 SENTINEL REMOVE unresolved-primary
        S 0 SENTINEL CONFIG SET announce-hostnames no
    }

    S 0 SENTINEL FLUSHCONFIG
    set c [split [read_file "sentinel_0/sentinel.conf"] "\n"]
    foreach h {replica-a.invalid replica-b.invalid} {
        assert_equal 1 [llength [lsearch -all -exact $c \
            "sentinel known-replica unresolved-primary $h 6379"]]
    }

    kill_instance sentinel 0
    restart_instance sentinel 0
    set h {}
    foreach r [S 0 SENTINEL REPLICAS unresolved-primary] {
        lappend h [dict get $r ip]
    }
    assert_equal {replica-a.invalid replica-b.invalid} [lsort $h]
}

# We need to revert any special configuration because all tests currently
# share the same instances.
test "(post-cleanup) Configure instances and sentinel for IPs" {
    set ::host "127.0.0.1"
    set_all_instances_hostname $::host
    set_redis_announce_ip $::host
    set_sentinel_config resolve-hostnames no
    set_sentinel_config announce-hostnames no
}
