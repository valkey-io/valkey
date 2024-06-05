# Check that sync method chosen by replica falls back to diskless in case of disk-based sync failure
	 
start_server {} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    start_server {} {
        set replica [srv 0 client]
        $replica config set repl-diskless-load diskless-fallback
    
        test "Sync method falls back to diskless sync after disk-based fails" {

            $replica config set key-load-delay 50
            $master config set client-output-buffer-limit "replica 1mb 1mb 60"

            #write to master so replica will have key to load during rdb load
            for {set i 0} {$i < 200000} {incr i} {
                $master set key:$i val:[concat "this is key :" ", $i!"]
            }

            set loglines [count_log_lines 0]
            $replica replicaof $master_host $master_port
            # wait for the replica to start reading the rdb

            wait_for_condition 1000 50 {
                    [string match {*replica_last_sync_type:disk*} [$replica info replication]] &&
                    [string match {*connected_clients:1*} [$master info clients]]
                } else {
                    fail "Replica didn't try disk based load to begin with"
            }

            wait_for_condition 1000 50 {
                    [string match {*loading:1*} [$replica info persistence]] 
                } else {
                    fail "Replica isn't loading"
            }

            # Writing more keys to the primary during sync to cause a COB overrun
            for {set i 0} {$i < 60000} {incr i} {
                $master set key:$i val:[concat "this is another key :" ", $i!"]
            }

             wait_for_condition 1000 50 {
                    [string match {*last_sync_aborted:1*} [$replica info replication]]
                } else {
                    fail "Primary didn't disconnect replica as expected"
            }
            
            # Validate the second sync attempt is from socket (parser)

            wait_for_condition 1000 50 {
                    [string match {*master_link_status:up*} [$replica info replication]]
                } else {
                    fail "Second sync attempt didn't succeeded from socket"
            }

            assert_match {*replica_last_sync_type:parser*} [$replica info replication]
            assert_match {*last_sync_aborted:0*} [$replica info replication]

        }
    }
}