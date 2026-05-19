start_server {tags {external:skip} overrides {cluster-enabled no}} {
    test "CLUSTERSCAN when not in cluster mode returns error" {
        # CLUSTERSCAN should not work when cluster mode disabled
        assert_error "*cluster support disabled*" {r clusterscan 0}
    }

    test "SCAN, HSCAN, SSCAN, ZSCAN rejects SLOT argument" {
        # SLOT is only valid for CLUSTERSCAN
        assert_error "*syntax*" {r scan 0 SLOT 0}

        r hset myhash field value
        assert_error "*syntax*" {r hscan myhash 0 SLOT 0}

        r sadd myset member
        assert_error "*syntax*" {r sscan myset 0 SLOT 0}

        r zadd myzset 1 member
        assert_error "*syntax*" {r zscan myzset 0 SLOT 0}
    }
}

start_cluster 1 0 {tags {external:skip cluster}} {
    test "CLUSTERSCAN with invalid cursor format" {
        # Invalid cursor formats should return error
        assert_error "*Invalid cursor*" {R 0 clusterscan "invalid"}
        assert_error "*Invalid cursor*" {R 0 clusterscan "abc-def-ghi"}

        # Missing parts of hashtag
        assert_error "*Invalid cursor*" {R 0 clusterscan "0-"}
        assert_error "*Invalid cursor*" {R 0 clusterscan "0-{0}"}
    }

    test "CLUSTERSCAN rejects invalid options" {
        set valid_cursor "0-{06S}-0"
        assert_error "*syntax*" {R 0 clusterscan $valid_cursor UNKNOWNOPTION}
        assert_error "*syntax*" {R 0 clusterscan $valid_cursor COUNT 10 BADOPTION}
        assert_error "*syntax*" {R 0 clusterscan 0 COUNT 0}
        assert_error "*syntax*" {R 0 clusterscan 0 COUNT -1}
        assert_error "*value is not an integer or out of range*" {R 0 clusterscan 0 COUNT not-an-integer}
        assert_error "*unknown type name*" {R 0 clusterscan 0 TYPE notatype}
    }

    test "CLUSTERSCAN with SLOT restricts to single slot" {
        # When SLOT X is provided, clusterscan should only iterate on slot X
        # and return "0" when exhausted and not advance to slot X+1.
        
        # Add a key to slot 0
        R 0 set "{06S}key" "value"
        
        # Scan slot 0 with SLOT argument
        set res [R 0 clusterscan 0 SLOT 0]
        set cursor [lindex $res 0]
        
        # Continue scanning until exhausted
        set max_loops 1000
        set iterations 0
        while {$cursor ne "0" && $iterations < $max_loops} {
            set res [R 0 clusterscan $cursor SLOT 0]
            set cursor [lindex $res 0]
            incr iterations
        }
        assert {$iterations < $max_loops}
        
        assert_equal $cursor "0"
    }
}

# CLUSTERSCAN Tests - 3-node cluster tests
start_cluster 3 0 {tags {external:skip cluster}} {
    test "CLUSTERSCAN basic functionality" {
        set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]

        # Populate keys
        set num_keys 100
        for {set i 0} {$i < $num_keys} {incr i} {
            set key "key:$i"
            $cluster set $key val
        }
        
        # Verify DBSIZE
        set total_keys 0
        foreach n {0 1 2} {
            incr total_keys [R $n dbsize]
        }
        assert {$total_keys == $num_keys}
        
        # Run CLUSTERSCAN
        set cursor "0"
        set scanned_keys {}
        set max_loops 20000

        set iterations 0
        while {$iterations < $max_loops} {
            set res [$cluster clusterscan $cursor]
            set cursor [lindex $res 0]
            set keys [lindex $res 1]

            foreach k $keys {
                lappend scanned_keys $k
            }

            if {$cursor eq "0"} { break }
            
            incr iterations
        }
        assert {$iterations < $max_loops}
        
        $cluster close

        # Verify all keys found
        set scanned_keys [lsort -unique $scanned_keys]
        assert_equal [llength $scanned_keys] $num_keys
    }

    test "CLUSTERSCAN with MATCH pattern" {
        set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]

        $cluster set "user:100" "val"
        $cluster set "user:200" "val"
        $cluster set "slot{06S}:100" "val"
        $cluster set "slot{06S}:200" "val"
        

        # Scan with MATCH pattern
        set cursor "0"
        set matched_keys {}
        set max_loops 20000 
        
        set iterations 0
        while {$iterations < $max_loops} {
            set res [$cluster clusterscan $cursor MATCH "user:*"]
            set cursor [lindex $res 0]
            set keys [lindex $res 1]

            foreach k $keys {
                lappend matched_keys $k
            }

            if {$cursor eq "0"} { break }
            
            incr iterations
        }

        assert {$iterations < $max_loops}

        # Verify only user:* keys matched
        set matched_keys [lsort -unique $matched_keys]
        foreach k $matched_keys {
            assert_match "user:*" $k
        }
        assert_equal [llength $matched_keys] 2

        # Verify slot match does not affect slot param
        set cursor 0-{06S}-0
        set res [$cluster clusterscan $cursor MATCH "slot:*" slot 0]
        set keys [lindex $res 1]

        foreach k $keys {
            assert_match "slot:*" $k
        }

        $cluster close
    }

    test "CLUSTERSCAN with single slot MATCH bypasses cluster walk" {
        set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]

        for {set i 0} {$i < 100} {incr i} {
            $cluster set "{5L5}-$i" "test"
        }

        set res [$cluster clusterscan 0 MATCH "{5L5}-*"]
        set cursor [lindex $res 0]
        set keys [lindex $res 1]

        assert_equal [llength $keys] 0
        assert_match "0-{5L5}-0" $cursor

        set found_keys {}
        set max_loops 1000
        set iterations 0
        while {$cursor ne "0" && $iterations < $max_loops} {
            # Assert cursor is in the slot matching the Match Pattern.
            assert_match "*-{5L5}-*" $cursor

            set res [$cluster clusterscan $cursor MATCH "{5L5}-*" COUNT 20]
            set cursor [lindex $res 0]
            foreach key [lindex $res 1] {
                lappend found_keys $key
            }

            incr iterations
        }

        assert {$iterations < $max_loops}

        set found_keys [lsort -unique $found_keys]
        assert_equal [llength $found_keys] 100
        foreach key $found_keys {
            assert_match "{5L5}-*" $key
        }

        $cluster close
    }

    test "CLUSTERSCAN Match slot redirects tests" {
        set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]
        # Test start from slot matching the Match Pattern
        set res [$cluster clusterscan "0" MATCH "{Qi}-*"]
        assert_equal [lindex $res 0] "0-{Qi}-0"
        assert_equal [llength [lindex $res 1]] 0

        # Test cursor is before the slot matching the Match Pattern
        set res [$cluster clusterscan "0-{06S}-0" MATCH "{Qi}-*"]
        assert_equal [lindex $res 0] "0-{Qi}-0"
        assert_equal [llength [lindex $res 1]] 0

        # Test cursor is past the slot matching the Match Pattern
        set res [$cluster clusterscan "0-{Qi}-0" MATCH "{06S}-*"]
        assert_equal [lindex $res 0] "0"
        assert_equal [llength [lindex $res 1]] 0

        $cluster close
    }

    test "CLUSTERSCAN concludes when SLOT and single slot MATCH mismatch" {
        set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]

        set res [$cluster clusterscan "0" SLOT 0 MATCH "{Qi}-*"]
        assert_equal [lindex $res 0] "0"
        assert_equal [llength [lindex $res 1]] 0

        set res [$cluster clusterscan "0-{06S}-0" SLOT 0 MATCH "{Qi}-*"]
        assert_equal [lindex $res 0] "0"
        assert_equal [llength [lindex $res 1]] 0

        $cluster close
    }

    test "CLUSTERSCAN with COUNT option" {
        set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]
        # COUNT is a hint, not a guarantee, but we can test it doesn't error
        set 0_slot_tag "{06S}"
        set 1_slot_tag "{Qi}"

        for {set i 0} {$i < 100} {incr i} {
            $cluster set "$0_slot_tag:$i" "value:$i"
            $cluster set "$1_slot_tag:$i" "value:$i" 
        }
        
        # Assert that with Count no keys when cursor is 0
        set cursor "0"
        set res [$cluster clusterscan $cursor COUNT 10]
        set keys [lindex $res 1]
        assert_equal [llength $keys] 0
        set cursor [lindex $res 0]

        # Assert to scan slot 0 only
        set max_loops 1000
        set iterations 0
        while {$cursor ne "0" && $iterations < $max_loops} {
            set res [$cluster clusterscan $cursor COUNT 10 SLOT 0]
            set cursor [lindex $res 0]
            foreach k [lindex $res 1] {
                # Assert that only slot 0 keys are returned
                assert {[string match "*$0_slot_tag*" $k]}
            }
            incr iterations
        }
        assert {$iterations < $max_loops}

        # Continue to scan the slot 1
        set res [$cluster clusterscan 0-$1_slot_tag-0 COUNT 30 SLOT 1]
        set keys [lindex $res 1]

        # Given that the count is a hint, result length is within a given range
        assert {[llength $keys] > 1}
        assert {[llength $keys] < 100}

        $cluster close
    }

    test "CLUSTERSCAN with TYPE filter" {
        # Add different data types
        set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]
        $cluster  set "string:test" "val"
        $cluster lpush "list:test" "a" "b"
        $cluster sadd "set:test" "x" "y"
        $cluster hset "hash:test" "f1" "v1"

        # Scan for only string type
        set cursor "0-{06S}-0"
        set max_loops 20000
        set iterations 0
        while {$cursor ne "0" && $iterations < $max_loops} {
            set res [$cluster clusterscan $cursor TYPE string]
            set cursor [lindex $res 0]
            foreach k [lindex $res 1] {
                # Assert that only string keys are being matched
                assert_match  [$cluster type $k] "string"
            }
            incr iterations
        }
        assert {$iterations < $max_loops}

        # Combine MATCH, COUNT, and TYPE
        for {set i 0} {$i < 100} {incr i} {
            $cluster set "string:{06S}:test:$i" "value:$i"
            $cluster set "alternatestring:{06S}:test:$i" "value:$i"
        }

        set cursor "0-{06S}-0"
        set res [$cluster clusterscan $cursor COUNT 40 MATCH alternate* TYPE string]
        set keys [lindex $res 1]

        # Given that the count is a hint, result length is within a given range
        assert {[llength $keys] > 1}
        assert {[llength $keys] < 100}

        foreach k $keys {
            # Assert that only string keys are being matched
            assert_match  [$cluster type $k] "string"
            assert_match "alternate*" $k
        }

        $cluster close   
    }

    test "CLUSTERSCAN empty result still returns valid cursor" {
        set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]
        set res [$cluster clusterscan 0]
        set cursor [lindex $res 0]
        # Cursor should match format: 0-{hashtag}-number
        assert_match "0-*-*" $cursor

        # Scan with impossible pattern - should return empty but valid cursor
        set cursor "0-{06S}-0"
        set res [$cluster clusterscan $cursor MATCH "impossible_pattern_xyz_*"]
        
        set new_cursor [lindex $res 0]
        set keys [lindex $res 1]
        # Keys should be empty
        assert_equal [llength $keys] 0

        # Cursor should still be valid format
        assert_match "*-*-*" $new_cursor
        
        $cluster close
    }

    test "CLUSTERSCAN fingerprint validation" {
        set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]

        # slot 0, 8192, 16000 are the three slots we will use for this test
        for {set i 0} {$i < 50} {incr i} {
            $cluster set "fingerprint:{06S}:$i" "value:$i"
            $cluster set "fingerprint:{8YG}:$i" "value:$i"
            $cluster set "fingerprint:{he}:$i" "value:$i"
        }

        set max_loops 1000
        set cursor "0"
        set keys {}
        set fps {}
        set iterations 0
        while {$iterations < $max_loops} {
            set res [$cluster clusterscan $cursor match "fingerprint:*" ]
            set cursor [lindex $res 0]
            foreach k [lindex $res 1] {
               lappend keys $k
            }
            incr iterations
            set fp [string range $cursor 0 [expr {[string first "-" $cursor] - 1}]]
            if {$fp ne "0"} {
                lappend fps $fp
            }

            if {$cursor eq "0"} {
                break
            }
        }
        assert {$iterations < $max_loops}

        # Assert all keys are returned
        assert_equal [llength $keys] 150

        # Assert different finger print for different slots
        set fps [lsort -unique $fps]
        assert {[llength $fps] >= 3}

        # Assert that the local cursor is ignored when finger print is 0
        set cursor "0-{he}-09393399393"
        set res [$cluster clusterscan $cursor match "fingerprint:*" count 100]
        set keys [lindex $res 1]
        assert_equal [llength $keys] 50

        $cluster close
    }

    # Verify CLUSTERSCAN range end at non 64-bit aligned slot boundary.
    # With 3 shards slot ownership is 0-5461, 5462-10922, 10923-16383.
    test "CLUSTERSCAN cursor advances to next shard at non 64 bit aligned boundary" {
        set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]

        set res [$cluster clusterscan 0-{06S}-0 COUNT 20000]
        set cur [lindex $res 0]
        assert_equal [$cluster cluster keyslot $cur] 5462
        $cluster close
    }
}

# CLUSTERSCAN Tests - 2-node cluster for MOVED redirection and SLOT argument
# We will not use the cluster client as we are validating the moved scenarios.
start_cluster 2 0 {tags {external:skip cluster}} {
    test "CLUSTERSCAN handles MOVED redirection" {
        # Find which node owns slot 0
        set slot0_owner 0
        set 0_slot_tag "{06S}"
        
        if {[catch {R 0 set "$0_slot_tag:test" "x"} err]} {
            if {[string match "MOVED*" $err]} {
                set slot0_owner 1
            }
        }
        R $slot0_owner del "$0_slot_tag:test"

        # Load 10 keys into slot 0 using {06S} hashtag
        set num_keys 10
        for {set i 0} {$i < $num_keys} {incr i} {
            R $slot0_owner set "$0_slot_tag:$i" "value:$i"
        }

        # Start scanning slot 0, collect some keys before migration.
        # Use COUNT 1 to get a partial scan so cursor is not "0".
        set keys_before_move {}
        set cursor "0-$0_slot_tag-0"
        set max_loops 100
        set iterations 0
        while {$iterations < $max_loops} {
            set res [R $slot0_owner clusterscan $cursor SLOT 0 COUNT 1]
            set cursor [lindex $res 0]
            foreach k [lindex $res 1] {
                lappend keys_before_move $k
            }
            # Stop once we have some keys but scan is not finished
            if {[llength $keys_before_move] > 0 && $cursor ne "0"} break
            if {$cursor eq "0"} {
                # Scan finished, restart
                set cursor "0-$0_slot_tag-0"
                set keys_before_move {}
            }
            incr iterations
        }
        assert {$cursor ne "0"}
        
        # Migrate slot 0 to the other node
        set target_node [expr {1 - $slot0_owner}]
        set source_id [R $slot0_owner CLUSTER MYID]
        set target_id [R $target_node CLUSTER MYID]
        
        R $slot0_owner CLUSTER SETSLOT 0 MIGRATING $target_id
        R $target_node CLUSTER SETSLOT 0 IMPORTING $source_id
        set target_port [lindex [R $target_node config get port] 1]
        if {$::tls} {
            set target_port [lindex [R $target_node config get tls-port] 1]
        }
        
        # Migrate all slot 0 keys
        for {set i 0} {$i < $num_keys} {incr i} {
            R $slot0_owner MIGRATE 127.0.0.1 $target_port "$0_slot_tag:$i" 0 10000
        }
        R $slot0_owner CLUSTER SETSLOT 0 NODE $target_id
        R $target_node CLUSTER SETSLOT 0 NODE $target_id
        
        # Original owner should return MOVED
        wait_for_condition 1000 50 {
            [catch {R $slot0_owner clusterscan $cursor} res] && [string match "MOVED 0 *" $res]
        } else {
            fail "Expected MOVED error"
        }
        
        # Continue scan on new owner until complete
        set keys_after_move {}
        set max_loops 1000
        set iterations 0
        while {$cursor ne "0" && $iterations < $max_loops} {
            set res [R $target_node clusterscan $cursor SLOT 0]
            set cursor [lindex $res 0]
            foreach k [lindex $res 1] {
                lappend keys_after_move $k
            }
            incr iterations
        }
        assert {$iterations < $max_loops}
        
        # Verify continuity: all 100 keys found across both scans
        set all_keys [concat $keys_before_move $keys_after_move]
        set all_keys [lsort -unique $all_keys]
        assert_equal [llength $all_keys] $num_keys
    }

    test "CLUSTERSCAN with SLOT argument error scenario" {
        # After the previous test, slot 0 was moved to target_node.
        # Find which node currently owns slot 0 for routing.
        set slot0_node 0
        if {[catch {R 0 clusterscan 0-{06S}-0 SLOT 0} res]} {
            set slot0_node 1
        }

        set cursor "0-{06S}-0"
        # Cursor has {06S} (slot 0) but SLOT says 1 -> mismatch
        assert_error "*Cursor slot mismatch*" {R $slot0_node clusterscan $cursor SLOT 1}

        # CLUSTERSCAN with invalid slot number
        assert_error "*Invalid or out of range slot*" {R $slot0_node clusterscan 0-{06S}-0 SLOT 20000}

        # CLUSTERSCAN with two SLOT option should result in error
        assert_error "*SLOT option can only be specified once*" {R $slot0_node clusterscan 0-{06S}-0 SLOT 0 SLOT 0}
    }

    test "CLUSTERSCAN range scanning and cursor hashtag correctness" {
        # slot 50 = {4ZG}, slot 100 = {0or} — both on node 0.
        R 0 set "{4ZG}:key" "value"
        R 0 set "{0or}:key" "value"

        # Both slots scanned in single call (range scan, default count > 2 keys)
        set res [R 0 clusterscan "0-{4ZG}-0"]
        set keys [lsort [lindex $res 1]]
        set cursor [lindex $res 0]

        # Both keys from slot 50 and 100 returned in one call
        assert_equal [llength $keys] 2
        assert_equal [lindex $keys 0] "{0or}:key"
        assert_equal [lindex $keys 1] "{4ZG}:key"

        # Cursor is cross-node transition
        assert_match "0-*-0" $cursor

        # Now scan happens one slot at a time
        set res [R 0 clusterscan "0-{4ZG}-0" COUNT 1]
        set keys [lindex $res 1]
        set cursor [lindex $res 0]

        # Found key from slot 50
        assert_equal [lindex $keys 0] "{4ZG}:key"

        # Cursor jumped to slot 100 (next non-empty slot), not slot 51.
        assert_match "*-{0or}-*" $cursor
        assert_not_equal $cursor "0"

        # Continue: scan slot 100
        set res [R 0 clusterscan $cursor]
        set keys [lindex $res 1]
        set cursor [lindex $res 0]

        assert_equal [lindex $keys 0] "{0or}:key"

        # Migrate slot 51 this breaks contiguous range at slot 50
        set source_id [R 0 cluster myid]
        set target_id [R 1 cluster myid]
        R 0 cluster setslot 51 migrating $target_id
        R 1 cluster setslot 51 importing $source_id
        R 0 cluster setslot 51 node $target_id
        R 1 cluster setslot 51 node $target_id

        # Re-scan from slot 50 — range now ends at 50 (slot 51 on node 1)
        set res [R 0 clusterscan "0-{4ZG}-0"]
        set keys [lindex $res 1]
        set cursor [lindex $res 0]

        assert_equal [lindex $keys 0] "{4ZG}:key"
        # Cross-node transition to slot 51 ({6od})
        assert_equal $cursor "0-{6od}-0"

        # Validate final slot of the cluster
        R 1 set "{6ZJ}:key" "value"
        set res [R 1 clusterscan "0-{6ZJ}-0"]
        set keys [lindex $res 1]
        set cursor [lindex $res 0]
        assert_equal [lindex $keys 0] "{6ZJ}:key"
        assert_equal $cursor "0"
    }
}

start_cluster 3 0 {tags {external:skip cluster}} {
    test "CLUSTERSCAN returns correct errors on cluster down and unassigned slots" {
        # Hashtag reference: {06S} -> slot 0 -> R0, {6ZJ} -> slot 16383 -> R2.

        # Case 1: Node 0 is paused, cluster enters FAIL state.
        # With cluster-require-full-coverage=yes (default), any CLUSTERSCAN
        # should get CLUSTERDOWN because the cluster cannot serve all slots.
        pause_process [srv 0 pid]
        wait_for_cluster_state fail
        assert_error {CLUSTERDOWN The cluster is down} {R 1 clusterscan "0-{06S}-0"}
        assert_error {CLUSTERDOWN The cluster is down} {R 1 clusterscan "0-{6ZJ}-0"}
        assert_error {CLUSTERDOWN The cluster is down} {R 2 clusterscan "0-{06S}-0"}
        assert_error {CLUSTERDOWN The cluster is down} {R 2 clusterscan "0-{6ZJ}-0"}

        # Case 2: Node 0 is paused, cluster enters FAIL state.
        # With cluster-require-full-coverage=no, full-coverage requirement disabled.
        # Now the cluster is "ok" even though node 0's slots are unreachable.
        # Cursors for slot 0 should get MOVED.
        R 1 config set cluster-require-full-coverage no
        R 2 config set cluster-require-full-coverage no
        wait_for_cluster_state ok
        # Node 0 owns slot 0, so this should get MOVED.
        assert_error {MOVED 0 *} {R 1 clusterscan "0-{06S}-0"}
        assert_error {MOVED 0 *} {R 2 clusterscan "0-{06S}-0"}
        # Slot 16383: assigned to node 2. Nodes that don't own it get MOVED;
        # node 2 handles it locally.
        assert_error {MOVED 16383 *} {R 1 clusterscan "0-{6ZJ}-0"}
        assert_equal [R 2 clusterscan "0-{6ZJ}-0"] {0 {}}

        # Restore full-coverage and bring node 0 back.
        R 1 config set cluster-require-full-coverage yes
        R 2 config set cluster-require-full-coverage yes
        resume_process [srv 0 pid]
        wait_for_cluster_state ok

        # Case 3: Delete slot 0 from all nodes, slot 0 is now unassigned.
        # With full-coverage=yes the cluster enters FAIL state.
        # Cursors for slot 0 should get "Hash slot not served".
        # Cursors for assigned but remote slots should get "cluster is down".
        R 0 CLUSTER DELSLOTS 0
        catch {R 1 CLUSTER DELSLOTS 0}
        catch {R 2 CLUSTER DELSLOTS 0}
        wait_for_cluster_state fail

        # Unassigned slot -> specific error.
        assert_error {CLUSTERDOWN Hash slot not served} {R 0 clusterscan "0-{06S}-0"}
        assert_error {CLUSTERDOWN Hash slot not served} {R 1 clusterscan "0-{06S}-0"}
        assert_error {CLUSTERDOWN Hash slot not served} {R 2 clusterscan "0-{06S}-0"}

        # Other slots are still assigned but cluster is in FAIL state.
        assert_error {CLUSTERDOWN The cluster is down} {R 0 clusterscan "0-{6ZJ}-0"}
        assert_error {CLUSTERDOWN The cluster is down} {R 1 clusterscan "0-{6ZJ}-0"}
        assert_error {CLUSTERDOWN The cluster is down} {R 2 clusterscan "0-{6ZJ}-0"}

        # Case 4: Disable full-coverage again with slot 0 still unassigned.
        # The cluster is "ok" but slot 0 remains unassigned.
        # Cursors for slot 0 should still get "Hash slot not served".
        # Cursors for assigned but remote slots should now get MOVED.
        R 0 config set cluster-require-full-coverage no
        R 1 config set cluster-require-full-coverage no
        R 2 config set cluster-require-full-coverage no
        wait_for_cluster_state ok

        # Slot 0: unassigned -> "Hash slot not served" regardless of node.
        assert_error {CLUSTERDOWN Hash slot not served} {R 0 clusterscan "0-{06S}-0"}
        assert_error {CLUSTERDOWN Hash slot not served} {R 1 clusterscan "0-{06S}-0"}
        assert_error {CLUSTERDOWN Hash slot not served} {R 2 clusterscan "0-{06S}-0"}

        # Slot 16383: assigned to node 2. Nodes that don't own it get MOVED;
        # node 2 handles it locally.
        assert_error {MOVED 16383 *} {R 0 clusterscan "0-{6ZJ}-0"}
        assert_error {MOVED 16383 *} {R 1 clusterscan "0-{6ZJ}-0"}
        # Node 2 owns slot 16383, so this should work.
        assert_equal [R 2 clusterscan "0-{6ZJ}-0"] {0 {}}
    }
}
