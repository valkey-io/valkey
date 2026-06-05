# Tests for valkey-cli --cluster compatibility with the Raft cluster bus.

source tests/support/cluster_util.tcl

tags {external:skip cluster singledb} {

start_server {overrides {cluster-enabled yes cluster-protocol raft}} {
    test "Raft cluster mutations cannot run inside MULTI/EXEC" {
        set r [srv 0 client]
        $r MULTI
        assert_equal {QUEUED} [$r CLUSTER ADDSLOTS 0]
        catch {$r EXEC} err
        assert {[string match {*not allowed inside MULTI*} $err] || [string match {*EXECABORT*} $err]}
    }
}

start_cluster 2 0 {tags {external:skip cluster}} {
    config_set_all_nodes cluster-allow-replica-migration no

    test "valkey-cli --cluster fix works on Raft cluster" {
        wait_for_cluster_state ok
        set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]
        array set nodefrom [$cluster masternode_for_slot 609]
        array set nodeto [$cluster masternode_notfor_slot 609]
        $cluster set aga xyz
        assert_equal {OK} [$nodefrom(link) cluster setslot 609 migrating $nodeto(id)]
        fix_cluster $nodefrom(addr)
        assert_equal "xyz" [$cluster get aga]
    }

    config_set_all_nodes cluster-allow-replica-migration yes
}

} ;# tags
