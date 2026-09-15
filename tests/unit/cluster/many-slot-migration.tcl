# Tests for many simultaneous migrations.

source tests/support/cluster_util.tcl

# TODO: This test currently runs without replicas, as failovers (which may
# happen on lower-end CI platforms) are still not handled properly by the
# cluster during slot migration (related to #6339).

start_cluster 10 0 {tags {external:skip cluster}} {
    config_set_all_nodes cluster-allow-replica-migration no

set key_count [expr {$::valgrind ? 10000 : 40000}]
set migration_slots [expr {$::valgrind ? 250 : 1000}]

test "Cluster is up" {
    wait_for_cluster_state ok
}

set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]
catch {unset nodefrom}
catch {unset nodeto}

$cluster refresh_nodes_map

test "Set many keys" {
    for {set i 0} {$i < $key_count} {incr i} {
        $cluster set key:$i val:$i
    }
}

test "Keys are accessible" {
    for {set i 0} {$i < $key_count} {incr i} {
        assert { [$cluster get key:$i] eq "val:$i" }
    }
}

test "Init migration of many slots" {
    # Valgrind makes cluster fix on 1000 half-migrated slots too slow for the
    # dedicated valgrind jobs. Keep the default coverage in normal runs while
    # still exercising simultaneous repair of many slots under valgrind.
    for {set slot 0} {$slot < $migration_slots} {incr slot} {
        array set nodefrom [$cluster masternode_for_slot $slot]
        array set nodeto [$cluster masternode_notfor_slot $slot]

        $nodefrom(link) cluster setslot $slot migrating $nodeto(id)
        $nodeto(link) cluster setslot $slot importing $nodefrom(id)
    }
}

test "Fix cluster" {
    wait_for_cluster_propagation
    fix_cluster $nodefrom(addr)
}

test "Keys are accessible" {
    for {set i 0} {$i < $key_count} {incr i} {
        assert { [$cluster get key:$i] eq "val:$i" }
    }
}

config_set_all_nodes cluster-allow-replica-migration yes

} ;# start_cluster
