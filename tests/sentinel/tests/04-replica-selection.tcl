# Test replica selection algorithm.
#
# This unit should test:
# 1) That when there are no suitable replicas, no failover is performed.
# 2) That among the available replicas, the one with better offset is picked.
source "../tests/includes/init-tests.tcl"

foreach_sentinel_id id {
    S $id SENTINEL DEBUG ping-period 500
    S $id SENTINEL DEBUG ask-period 500
    S $id SENTINEL DEBUG info-period 500
    S $id SENTINEL DEBUG default-down-after 1000
}

# This unit is the only one in the whole sentinel test suite that
# requires two clusters. Here we will mainly operate on the second cluster.

# Spawn 1 primary with only 1 replica
set num_instances 2
spawn_instance valkey $::valkey_base_port $num_instances {
    "enable-protected-configs yes"
    "enable-debug-command yes"
    "save ''"
}

set primary_a_id 0
set primary_a_name "mymaster"
# The first 5 IDs belong to the default primary-replica cluster
set primary_b_id $::instances_count
set primary_b_name "another_primary"
set replica_id [expr $primary_b_id + 1]

# Create the second cluster
init_cluster $primary_b_name $primary_b_id $num_instances

# Start tests
test "The second cluster works" {
    # Put a simple string into the database
    R $primary_b_id SET "mykey" "myvalue"

    wait_for_condition 200 50 {
        [R $replica_id GET "mykey"] == "myvalue"
    } else {
        fail "The replica and primary in the second cluster cannot sync"
    }
}

# Now that we have two clusters, we need to do proper cleanup
# to avoid messing up other test suites.
foreach_sentinel_id id {
    S $id SENTINEL REMOVE $primary_b_name
}
remove_valkey_instance [list $primary_b_id $replica_id]