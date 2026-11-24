# Test ACL permissions for cluster commands

tags {tls:skip external:skip cluster} {
    start_cluster 2 0 {tags {"external:skip cluster acl"}} {
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

        test {Test CLUSTER FLUSHSLOT with database permissions} {
            R 0 ACL SETUSER cluster-flushslot-user on nopass +cluster +select ~* db=0

            set r2 [valkey [srv 0 host] [srv 0 port] 0 $::tls]
            $r2 auth cluster-flushslot-user ""
            $r2 select 0

            set key_slot [R 0 CLUSTER KEYSLOT FC]

            assert_error "*NOPERM*database*" {$r2 cluster flushslot $key_slot}
            assert_error "*NOPERM*database*" {$r2 cluster flushslot $key_slot SYNC}
            assert_error "*NOPERM*database*" {$r2 cluster flushslot $key_slot ASYNC}

            R 0 ACL SETUSER cluster-flushslot-user alldbs

            assert_equal {OK} [$r2 cluster flushslot $key_slot]

            $r2 close
            R 0 ACL DELUSER cluster-flushslot-user
        }
    }
}
