# A replica claim that would close a replicaof cycle in our view must not
# be adopted. An observer receives forged replica-claim PINGs from paused
# nodes, sent from a plain socket as in failover-nack.tcl. Adopting the
# second claim used to set A->B->A in the observer, and then
# clusterNodeGetPrimary() asserted (primary->replicaof == NULL) in
# updateShardId(). See the fix for #5439.

# Build a PING from `sender_name` that claims to be a replica of `primary_name`.
# `extra_flags` is OR-ed into the header flags. The observer copies them onto
# the sender, so the test can wait for a marker after the packet is handled.
proc create_replica_claim_ping {sender_name sender_port sender_cport primary_name extra_flags} {
    set CLUSTER_NAMELEN 40
    set CLUSTER_SLOTS 16384
    set NET_IP_STR_LEN 46
    set CLUSTER_NODE_REPLICA 2

    set packet ""
    append packet "RCmb"
    append packet [binary format I 0]                      ;# totlen, patched below
    append packet [binary format S 1]                      ;# ver
    append packet [binary format S $sender_port]           ;# port
    append packet [binary format S 0]                      ;# type: PING
    append packet [binary format S 0]                      ;# count
    append packet [binary format W 0]                      ;# currentEpoch
    append packet [binary format W 0]                      ;# configEpoch
    append packet [binary format W 0]                      ;# offset
    append packet [string range "${sender_name}[string repeat "\x00" $CLUSTER_NAMELEN]" 0 [expr {$CLUSTER_NAMELEN - 1}]]
    append packet [string repeat "\x00" [expr {$CLUSTER_SLOTS / 8}]] ;# myslots
    append packet [string range "${primary_name}[string repeat "\x00" $CLUSTER_NAMELEN]" 0 [expr {$CLUSTER_NAMELEN - 1}]]
    append packet [string repeat "\x00" $NET_IP_STR_LEN]   ;# myip
    append packet [binary format S 0]                      ;# extensions
    append packet [string repeat "\x00" 30]                ;# notused1
    append packet [binary format S 0]                      ;# pport
    append packet [binary format S $sender_cport]          ;# cport
    append packet [binary format S [expr {$CLUSTER_NODE_REPLICA | $extra_flags}]]
    append packet [binary format c 0]                      ;# state
    append packet [binary format ccc 0 0 0]                ;# mflags
    return [string replace $packet 4 7 [binary format I [string length $packet]]]
}

# Send `packet` to the cluster bus of instance `id`.
proc send_to_cluster_bus {id packet} {
    set sock [socket 127.0.0.1 [expr {[srv -$id port] + 10000}]]
    fconfigure $sock -translation binary -buffering none -blocking 1
    puts -nonewline $sock $packet
    flush $sock
    close $sock
}

# Make instance `obs` in the cluster view say that `from_id` is a replica of
# `to_id`. `from_idx`/`to_idx` are the instance indexes of those nodes.
proc forge_replica_claim {obs from_idx to_idx marker} {
    set NOFAILOVER 512
    set flags [expr {$marker ? $NOFAILOVER : 0}]
    set packet [create_replica_claim_ping $::cid($from_idx) [srv -$from_idx port] \
        [expr {[srv -$from_idx port] + 10000}] $::cid($to_idx) $flags]
    send_to_cluster_bus $obs $packet
}

proc view_primary_of {obs node_idx} {
    set node [cluster_get_node_by_id $obs $::cid($node_idx)]
    return [dict get $node slaveof]
}

proc view_has_flag {obs node_idx flag} {
    set node [cluster_get_node_by_id $obs $::cid($node_idx)]
    return [expr {[lsearch -exact [dict get $node flags] $flag] != -1}]
}

# Instance indexes: R0 and R1 are primaries. R2, R4 and R6 replicate R0.
# R3, R5 and R7 replicate R1.
start_cluster 2 6 {tags {external:skip cluster tls:skip}} {
    set obs 1
    set p 0
    for {set i 0} {$i < 8} {incr i} {
        set ::cid($i) [R $i cluster myid]
    }

    test "Replica claim closing a two-node cycle is not adopted" {
        set a 2
        set b 4
        assert_equal $::cid($p) [view_primary_of $obs $a]
        assert_equal $::cid($p) [view_primary_of $obs $b]

        pause_process [srv -$a pid]
        pause_process [srv -$b pid]

        # In the observer's view, A now replicates B.
        forge_replica_claim $obs $a $b 0
        wait_for_condition 100 50 {
            [view_primary_of $obs $a] eq $::cid($b)
        } else {
            fail "A -> B claim was not applied"
        }

        # B claims A. That would close A -> B -> A, so it must be skipped.
        # The NOFAILOVER marker shows that the packet was processed.
        forge_replica_claim $obs $b $a 1
        wait_for_condition 100 50 {
            [view_has_flag $obs $b nofailover]
        } else {
            fail "B -> A claim was not processed"
        }
        assert_equal PONG [R $obs ping]
        assert_equal $::cid($p) [view_primary_of $obs $b]
        assert_equal $::cid($b) [view_primary_of $obs $a]

        resume_process [srv -$a pid]
        resume_process [srv -$b pid]
        # The real packets from A and B restore the true topology.
        wait_for_condition 100 100 {
            [view_primary_of $obs $a] eq $::cid($p) &&
            [view_primary_of $obs $b] eq $::cid($p)
        } else {
            fail "observer view did not converge after resume"
        }
    }

    test "Replica claim closing a three-node cycle is not adopted" {
        set a 2
        set b 4
        set c 6
        wait_for_condition 100 100 {
            ![view_has_flag $obs $b nofailover]
        } else {
            fail "nofailover marker on B was not cleared"
        }

        pause_process [srv -$a pid]
        pause_process [srv -$b pid]
        pause_process [srv -$c pid]

        forge_replica_claim $obs $a $b 0
        forge_replica_claim $obs $b $c 0
        wait_for_condition 100 50 {
            [view_primary_of $obs $a] eq $::cid($b) &&
            [view_primary_of $obs $b] eq $::cid($c)
        } else {
            fail "A -> B -> C chain was not built"
        }

        # C claims A. That would close A -> B -> C -> A, so it must be skipped.
        forge_replica_claim $obs $c $a 1
        wait_for_condition 100 50 {
            [view_has_flag $obs $c nofailover]
        } else {
            fail "C -> A claim was not processed"
        }
        assert_equal PONG [R $obs ping]
        assert_equal $::cid($p) [view_primary_of $obs $c]

        resume_process [srv -$a pid]
        resume_process [srv -$b pid]
        resume_process [srv -$c pid]
        wait_for_condition 100 100 {
            [view_primary_of $obs $a] eq $::cid($p) &&
            [view_primary_of $obs $b] eq $::cid($p) &&
            [view_primary_of $obs $c] eq $::cid($p)
        } else {
            fail "observer view did not converge after resume"
        }
    }
}

# A replicaof cycle saved in nodes.conf, for example by an older version that
# could build one from stale gossip, must be refused at load. Otherwise a later
# claim from C naming A as its primary links C under the A <-> B cycle, and
# clusterNodeGetPrimary(C) never returns.
start_cluster 2 6 {tags {external:skip cluster tls:skip}} {
    # R3 is a replica of R1. Restarting a replica does not trigger a failover.
    set obs 3
    for {set i 0} {$i < 8} {incr i} {
        set ::cid($i) [R $i cluster myid]
    }

    test "Replicaof cycle in nodes.conf is refused at startup" {
        set a 2
        set b 4

        # Copy the observer's nodes.conf with A -> B and B -> A written in,
        # and start a separate server on the copy.
        set new_lines {}
        foreach line [split [read_file [cluster_nodes_conf_path $obs]] "\n"] {
            set fields [split $line " "]
            if {[lindex $fields 0] eq $::cid($a)} {
                set line [join [lreplace $fields 3 3 $::cid($b)] " "]
            } elseif {[lindex $fields 0] eq $::cid($b)} {
                set line [join [lreplace $fields 3 3 $::cid($a)] " "]
            }
            lappend new_lines $line
        }
        set dir [file normalize [tmpdir replicaof-cycle]]
        write_file [file join $dir nodes-cycle.conf] [join $new_lines "\n"]

        set logfile [file join $dir cycle-node.log]
        set pid [exec $::VALKEY_SERVER_BIN --port 0 \
                     --unixsocket [file join $dir cycle-node.sock] \
                     --cluster-enabled yes \
                     --dir $dir \
                     --cluster-config-file nodes-cycle.conf \
                     --logfile $logfile &]
        # Wait for the panic message instead of the exit: with core dumps
        # enabled, the exit after a panic can take a long time.
        wait_for_condition 100 100 {
            [file exists $logfile] &&
            [string match "*corrupted cluster config file*" [read_file $logfile]]
        } else {
            catch {exec kill -9 $pid}
            fail "server started with a replicaof cycle in nodes.conf"
        }
        catch {exec kill -9 $pid}
        set log [read_file $logfile]
        assert_no_match "*Ready to accept connections*" $log
        assert_match "*Replicaof cycle detected*" $log
    }
}
