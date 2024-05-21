source ../tests/includes/init-tests.tcl

test "Create a 3-node cluster" {
    create_cluster 3 0
}

test "Cluster is up" {
    assert_cluster_state ok
}

set cluster [valkey_cluster 127.0.0.1:[get_instance_attrib valkey 0 port]]

test "Initialize stream and blocked client" {
    $cluster xgroup create stream mygroup $ MKSTREAM
    # stream maps to 1386
    set slot 1386
    # Start a background process to simulate a blocked client on XREADGROUP
    set clientpid [exec sh -c "echo 'xreadgroup GROUP mygroup consumer BLOCK 0 streams stream >' | ../../../src/valkey-cli -c -p [get_instance_attrib valkey 0 port]" &]
    after 1000
}

test "Regression test for a crash where a node is blocked on XREADGROUP and the stream's slot is migrated" {
    # Find which node owns the slot
    set source_info [get_slot_owner_info 0 $slot]
    set source_cluster_id [dict get $source_info id]
    set source [dict get $source_info config_epoch]

    # Calculate the target node using a cyclic sequence
    set target [expr {($source % 3) + 1}]
    set target_cluster_id [exec echo [R 0 cluster nodes] | awk "\$7 == $target" | awk {{print $1}}]
    
    # Migrate the slot to the target node
    R [expr {$source - 1}] cluster setslot $slot migrating $target_cluster_id
    R [expr {$target - 1}] cluster setslot $slot importing $source_cluster_id
    # This line should cause the crash
    puts [$cluster migrate 127.0.0.1 [get_instance_attrib valkey [expr {$target - 1}] port] stream 0 5000]
}

