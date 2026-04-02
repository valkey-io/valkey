# Test ACL permissions for cluster commands

tags {tls:skip external:skip cluster} {
    set base_conf [list cluster-enabled yes cluster-databases 16]
    start_multiple_servers 3 [list overrides $base_conf] {

        test "Cluster nodes are reachable" {
            for {set id 0} {$id < [llength $::servers]} {incr id} {
                wait_for_condition 1000 50 {
                    ([catch {R $id ping} ping_reply] == 0) &&
                    ($ping_reply eq {PONG})
                } else {
                    catch {R $id ping} err
                    fail "Node #$id keeps replying '$err' to PING."
                }
            }
        }

        test "Cluster Join and auto-discovery test" {
            for {set attempts 3} {$attempts > 0} {incr attempts -1} {
                if {[join_nodes_in_cluster] == 1} {
                    break
                }
            }
            if {$attempts == 0} {
                fail "Cluster failed to form full mesh"
            }
        }

        test "Allocate slots to nodes" {
            cluster_allocate_slots 3 0
            wait_for_cluster_state ok
        }

        test {Test CLUSTER MIGRATESLOTS with database permissions} {
            set target_id [R 1 CLUSTER MYID]
            
            R 0 ACL SETUSER cluster-migrate-user on nopass +cluster +select ~* db=0
            
            set r2 [valkey [srv 0 host] [srv 0 port] 0 $::tls]
            $r2 auth cluster-migrate-user ""
            $r2 select 0
            
            catch {$r2 cluster migrateslots slotsrange 100 100 node $target_id} e
            assert_match "*NOPERM*database*" $e
            
            R 0 ACL SETUSER cluster-migrate-user alldbs
            
            catch {$r2 cluster migrateslots slotsrange 100 100 node $target_id} e
            assert {![string match "*NOPERM*database*" $e]}
            
            $r2 close
            R 0 ACL DELUSER cluster-migrate-user
        }

        test {Test CLUSTER CANCELSLOTMIGRATIONS with database permissions} {
            R 0 ACL SETUSER cluster-cancel-user on nopass +cluster +select ~* db=0
            
            set r2 [valkey [srv 0 host] [srv 0 port] 0 $::tls]
            $r2 auth cluster-cancel-user ""
            $r2 select 0
            
            catch {$r2 cluster cancelslotmigrations} e
            assert_match "*NOPERM*database*" $e
            
            R 0 ACL SETUSER cluster-cancel-user alldbs
            
            catch {$r2 cluster cancelslotmigrations} e
            assert {![string match "*NOPERM*database*" $e]}
            
            $r2 close
            R 0 ACL DELUSER cluster-cancel-user
        }
    }
}
