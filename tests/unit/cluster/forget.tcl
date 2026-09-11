# Tests that a node removed with CLUSTER FORGET stays removed.
# Since the no-inbound-link MEET retry (#1307), a forgotten node that is
# still running keeps sending MEET packets to the cluster and used to
# reintroduce itself once the blacklist TTL expired (#2788).

start_cluster 2 1 {tags {external:skip cluster} overrides {cluster-node-timeout 1000}} {
    test "FORGET-ed node does not reintroduce itself via MEET" {
        set forgotten_id [dict get [get_myself 2] id]

        # Use a short blacklist TTL so the test observes behavior past its
        # expiration, when the forgotten node used to be re-accepted.
        R 0 CONFIG SET cluster-blacklist-ttl 8
        R 1 CONFIG SET cluster-blacklist-ttl 8

        R 0 CLUSTER FORGET $forgotten_id
        R 1 CLUSTER FORGET $forgotten_id
        assert {[cluster_get_node_by_id 0 $forgotten_id] eq {}}
        assert {[cluster_get_node_by_id 1 $forgotten_id] eq {}}

        # Node 2 still knows the cluster and has outbound links but no
        # inbound links, so it will try to MEET its way back in. Poll well
        # past several handshake timeouts and the blacklist expiration; the
        # node must stay forgotten the whole time.
        for {set i 0} {$i < 30} {incr i} {
            after 500
            assert {[cluster_get_node_by_id 0 $forgotten_id] eq {}}
            assert {[cluster_get_node_by_id 1 $forgotten_id] eq {}}
        }

        # The node stayed out because its MEET packets were refused,
        # not because it never tried.
        verify_log_message 0 "*Ignoring MEET packet from blacklisted node $forgotten_id*" 0
        verify_log_message -1 "*Ignoring MEET packet from blacklisted node $forgotten_id*" 0
    }
}
