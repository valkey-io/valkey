# Tests for fixing migrating slot at all stages:
# 1. when migration is half inited on "migrating" node
# 2. when migration is half inited on "importing" node
# 3. migration inited, but not finished
# 4. migration is half finished on "migrating" node
# 5. migration is half finished on "importing" node

source tests/support/cluster_util.tcl

# Returns 1 if the node behind 'link' sees 'slot' as unassigned.
proc slot_is_unassigned {link slot} {
    foreach range [$link cluster slots] {
        if {$slot >= [lindex $range 0] && $slot <= [lindex $range 1]} {
            return 0
        }
    }
    return 1
}

start_cluster 2 0 {tags {external:skip cluster}} {

config_set_all_nodes cluster-allow-replica-migration no

test "Cluster is up" {
    wait_for_cluster_state ok
}

set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]
catch {unset nodefrom}
catch {unset nodeto}

proc reset_cluster {} {
    uplevel 1 {
        $cluster refresh_nodes_map
        array set nodefrom [$cluster masternode_for_slot 609]
        array set nodeto [$cluster masternode_notfor_slot 609]
    }
}

reset_cluster

$cluster set aga xyz

test "Half init migration in 'migrating' is fixable" {
    assert_equal {OK} [$nodefrom(link) cluster setslot 609 migrating $nodeto(id)]
    fix_cluster $nodefrom(addr)
    assert_equal "xyz" [$cluster get aga]
}

test "Half init migration in 'importing' is fixable" {
    assert_equal {OK} [$nodeto(link) cluster setslot 609 importing $nodefrom(id)]
    fix_cluster $nodefrom(addr)
    assert_equal "xyz" [$cluster get aga]
}

test "Uncovered slot is reassigned by cluster fix" {
    reset_cluster
    assert_equal {OK} [$cluster set aga xyz]
    assert_equal {OK} [$nodefrom(link) cluster delslots 609]
    fix_cluster $nodefrom(addr)
    assert_equal "xyz" [$cluster get aga]
    reset_cluster
}

test "Uncovered slot with keys in two nodes is fixed without losing keys" {
    reset_cluster
    assert_equal {OK} [$cluster set aga xyz]
    assert_equal {OK} [$cluster set "{aga}1" one]
    # Put a key of the same slot on the other node too, through a temporary
    # IMPORTING state.
    assert_equal {OK} [$nodeto(link) cluster setslot 609 importing $nodefrom(id)]
    assert_equal {OK} [$nodeto(link) asking]
    assert_equal {OK} [$nodeto(link) set "{aga}2" two]
    assert_equal {OK} [$nodeto(link) cluster setslot 609 stable]
    assert_equal 2 [$nodefrom(link) cluster countkeysinslot 609]
    assert_equal 1 [$nodeto(link) cluster countkeysinslot 609]
    # Make the slot uncovered. Gossip doesn't propagate DELSLOTS, so send it to
    # both nodes. With Raft, the second one is a no-op or fails because the slot
    # is already unassigned.
    assert_equal {OK} [$nodefrom(link) cluster delslots 609]
    catch {$nodeto(link) cluster delslots 609}
    wait_for_condition 100 50 {
        [slot_is_unassigned $nodefrom(link) 609] && [slot_is_unassigned $nodeto(link) 609]
    } else {
        fail "Slot 609 is still assigned"
    }
    # Fix moves all keys to one node, which becomes the owner.
    fix_cluster $nodefrom(addr)
    reset_cluster
    assert_equal 3 [$nodefrom(link) cluster countkeysinslot 609]
    assert_equal 0 [$nodeto(link) cluster countkeysinslot 609]
    assert_equal "xyz" [$cluster get aga]
    assert_equal "one" [$cluster get "{aga}1"]
    assert_equal "two" [$cluster get "{aga}2"]
    assert_equal 2 [$nodefrom(link) del "{aga}1" "{aga}2"]
}

test "SETSLOT NODE to the current owner keeps migrating state" {
    reset_cluster
    assert_equal {OK} [$nodefrom(link) cluster setslot 609 migrating $nodeto(id)]
    assert {[string first "\[609->-$nodeto(id)\]" [$nodefrom(link) cluster nodes]] >= 0}
    assert_equal {OK} [$nodeto(link) cluster setslot 609 node $nodefrom(id)]
    assert {[string first "\[609->-$nodeto(id)\]" [$nodefrom(link) cluster nodes]] >= 0}
    assert_equal {OK} [$nodefrom(link) cluster setslot 609 stable]
}

test "DELSLOTS to a non-owner does not unassign the slot or delete keys" {
    reset_cluster
    $cluster set aga xyz
    assert_equal {OK} [$nodeto(link) cluster delslots 609]
    wait_for_cluster_propagation
    assert_equal "xyz" [$cluster get aga]
}

test "Init migration and move key" {
    assert_equal {OK} [$nodefrom(link) cluster setslot 609 migrating $nodeto(id)]
    assert_equal {OK} [$nodeto(link) cluster setslot 609 importing $nodefrom(id)]
    assert_equal {OK} [$nodefrom(link) migrate $nodeto(host) $nodeto(port) aga 0 10000]
    wait_for_cluster_propagation
    assert_equal "xyz" [$cluster get aga]
    fix_cluster $nodefrom(addr)
    assert_equal "xyz" [$cluster get aga]
}

reset_cluster

test "Move key again" {
    wait_for_cluster_propagation
    assert_equal {OK} [$nodefrom(link) cluster setslot 609 migrating $nodeto(id)]
    assert_equal {OK} [$nodeto(link) cluster setslot 609 importing $nodefrom(id)]
    assert_equal {OK} [$nodefrom(link) migrate $nodeto(host) $nodeto(port) aga 0 10000]
    wait_for_cluster_propagation
    assert_equal "xyz" [$cluster get aga]
}

test "Half-finish migration" {
    # half finish migration on 'migrating' node
    assert_equal {OK} [$nodefrom(link) cluster setslot 609 node $nodeto(id)]
    fix_cluster $nodefrom(addr)
    assert_equal "xyz" [$cluster get aga]
}

reset_cluster

test "Move key back" {
    # 'aga' key is in 609 slot
    assert_equal {OK} [$nodefrom(link) cluster setslot 609 migrating $nodeto(id)]
    assert_equal {OK} [$nodeto(link) cluster setslot 609 importing $nodefrom(id)]
    assert_equal {OK} [$nodefrom(link) migrate $nodeto(host) $nodeto(port) aga 0 10000]
    assert_equal "xyz" [$cluster get aga]
}

test "Half-finish importing" {
    # Now we half finish 'importing' node
    assert_equal {OK} [$nodeto(link) cluster setslot 609 node $nodeto(id)]
    wait_for_condition 1000 50 {
        [string first "\[609-<-" [$nodeto(link) cluster nodes]] < 0 &&
        [string first "\[609->-" [$nodefrom(link) cluster nodes]] < 0
    } else {
        fail "Leftover IMPORTING/MIGRATING was not cleared after ownership change"
    }
    fix_cluster $nodefrom(addr)
    assert_equal "xyz" [$cluster get aga]
}

config_set_all_nodes cluster-allow-replica-migration yes

} ;# start_cluster
