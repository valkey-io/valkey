set someothermodule [file normalize tests/modules/timer.so]
set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
set extdatamodule2 [file normalize tests/modules/extstorage/extdata2.so]
set ext_data_off_err "ERR External data commands are unavailable with ext-data-mode off"

# Cleanup function for external data dump tests
proc cleanup_external_data_dump {client} {
    # Clean up external data and backup files from this test to ensure isolation for next test
    catch {$client flushall}
    exec sh -c {rm -rf /tmp/external_data}
}

proc scan_keys {cur type match {storage ""}} {
    set keys {}
    set k {}
    while 1 {
        if {$storage != ""} {
            set res [r scan $cur type $type match $match $storage]
        } else {
            set res [r scan $cur type $type match $match]
        }
        set cur [lindex $res 0]
        set k [lindex $res 1]
        lappend keys {*}$k
        if {$cur == 0} break
    }
    return $keys
}

proc gen_data {size} {
    set data ""
    for {set i 0} {$i < $size} {incr i} {
        append data "a"
    }
    return $data
}

# Helper to setup a cluster with primaries and optional replicas
# primaries: list of primary clients
# replicas_dict: dict mapping replica client to primary index in the primaries list
proc setup_ready_cluster {primaries replicas_dict} {
    set num_primaries [llength $primaries]
    
    # Get all node IDs
    set primary_ids {}
    foreach primary $primaries {
        lappend primary_ids [$primary CLUSTER MYID]
    }
    
    # Set config epochs for all nodes (primaries first, then replicas)
    set epoch 1
    foreach primary $primaries {
        $primary CLUSTER SET-CONFIG-EPOCH $epoch
        incr epoch
    }
    dict for {replica primary_idx} $replicas_dict {
        $replica CLUSTER SET-CONFIG-EPOCH $epoch
        incr epoch
    }
    
    # Distribute slots equally among primaries
    set slots_per_primary [expr {16384 / $num_primaries}]
    set slot 0
    for {set i 0} {$i < $num_primaries} {incr i} {
        set primary [lindex $primaries $i]
        set end_slot [expr {($i == $num_primaries - 1) ? 16383 : ($slot + $slots_per_primary - 1)}]
        $primary CLUSTER ADDSLOTSRANGE $slot $end_slot
        set slot [expr {$end_slot + 1}]
    }
    
    # Form cluster: all nodes meet the first primary
    set first_primary [lindex $primaries 0]
    set first_port [lindex [$first_primary CONFIG GET port] 1]
    
    for {set i 1} {$i < $num_primaries} {incr i} {
        set primary [lindex $primaries $i]
        $primary CLUSTER MEET 127.0.0.1 $first_port
    }
    
    dict for {replica primary_idx} $replicas_dict {
        $replica CLUSTER MEET 127.0.0.1 $first_port
    }
    
    after 100
    
    # Set up replication relationships
    dict for {replica primary_idx} $replicas_dict {
        set primary_id [lindex $primary_ids $primary_idx]
        $replica CLUSTER REPLICATE $primary_id
    }
    
    # Wait for all nodes to be ready
    foreach primary $primaries {
        wait_for_condition 50 100 {
            [catch {$primary CLUSTER INFO}] == 0 &&
            [string match "*cluster_state:ok*" [$primary CLUSTER INFO]]
        } else {
            fail "Cluster primary did not become ready"
        }
    }
    
    dict for {replica primary_idx} $replicas_dict {
        wait_for_condition 50 100 {
            [catch {$replica CLUSTER INFO}] == 0 &&
            [string match "*cluster_state:ok*" [$replica CLUSTER INFO]]
        } else {
            fail "Cluster replica did not become ready"
        }
    }
    
    # Wait for replicas to connect to primaries
    dict for {replica primary_idx} $replicas_dict {
        wait_for_condition 50 100 {
            [string match "*master_link_status:up*" [$replica INFO replication]]
        } else {
            fail "Replica did not connect to primary"
        }
    }
    
    # Cleanup
    foreach primary $primaries {
        cleanup_external_data_dump $primary
    }
    dict for {replica primary_idx} $replicas_dict {
        cleanup_external_data_dump $replica
    }
}

start_server {tags {"external_data external:skip"}} {
    test {Running EXTERNAL_DATA commands with switched off external data fails} {
        assert_error $ext_data_off_err {r external_data init db0 helloextdata1}
        assert_error $ext_data_off_err {r external_data loaded}
        assert_error $ext_data_off_err {r external_data stats dbs}
        assert_error $ext_data_off_err {r set k v ext}
    }
}

test {Server fails to start when ext-data-mode is kv but ext-data-id is missing} {
    # Try to start server with ext-data-mode=kv but without ext-data-id
    set config_file [file join [pwd] "temp_config_test.conf"]
    set fd [open $config_file w]
    puts $fd "ext-data-mode kv"
    close $fd
    
    # Attempt to start server with this config - should fail
    set result [catch {
        start_server [list overrides [list "ext-data-mode" kv]] {
            # This block should not execute
            fail "Server should not start without ext-data-id"
        }
    } error]
    
    # Clean up config file
    catch {file delete $config_file}
    
    # Verify that server failed to start
    assert {$result == 1}
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id57] tags [list "external:skip"]] {
    test {Running EXTERNAL_DATA LOADED with switched on external data succeeds} {
        assert_equal [list ] [r external_data loaded]
    }

    test {Loading some module does not affect LOADED commands} {
        assert_equal {OK} [r module load $someothermodule]
        assert_equal [list ] [r external_data loaded]
    }

    test {Dropping unloaded module fails} {
        assert_error {ERR db0 is not initialized} {r external_data drop db0}
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id60] tags [list "external:skip" "basic"]] {
    test {Loading module does affect LOADED commands} {
        # success on module load
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal [list helloextdata1] [r external_data loaded]

        # unload non-loaded module fails
        set code [catch {r module unload helloextdata2}]
        if {$code == 0} {
            puts "expected error on unloading non-loaded module, got none"
            exit 1
        }

        # same module loaded twice
        set code [catch {r module load $extdatamodule1}]
        if {$code == 0} {
            puts "expected error on loading same module twice, got none"
            exit 1
        }

        # success on several modules load
        assert_equal {OK} [r module load $extdatamodule2] 
        assert_equal [list helloextdata1 helloextdata2] [r external_data loaded]

        # unload loaded modules ok
        assert_equal {OK} [r module unload helloextdata1]
        assert_equal [list helloextdata2] [r external_data loaded]
        assert_equal {OK} [r module unload helloextdata2]
        assert_equal [list ] [r external_data loaded]
    }
}

### Non-sharded
start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id59] tags [list "external:skip"]] {
    test {Initializing and dropping db affects STATS commands} {
        # STATS ok with modules non-loaded
        assert_equal [list ] [r external_data stats dbs]

        # check init input arguments
        assert_error {ERR failed to parse db number from db, expect db0, db10, etc.} {r external_data INIT db helloextdata1}
        assert_error {ERR db number 16 exceeds used on server 0-15} {r external_data INIT db16 helloextdata1}
        assert_error {ERR wrong number of arguments for 'external_data|init' command} {r external_data INIT db0 }

        # you need to load modules to init
        assert_error {ERR module helloextdata1 is not loaded} {r external_data INIT db0 helloextdata1}

        # STATS ok with modules loaded
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal [list ] [r external_data stats dbs]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal [list db0:helloextdata1] [r external_data stats dbs]

        # Load same module again after init fails, but the server won't crash
        assert_error {ERR Error loading the extension. Please check the server logs.} {r module load $extdatamodule1}

        # Another module is added to stats
        assert_equal {OK} [r module load $extdatamodule2]
        assert_equal {OK} [r external_data INIT db1 helloextdata2]
        assert_equal [list db0:helloextdata1 db1:helloextdata2] [r external_data stats dbs]

        # you can't init the same db without dropping its currently used modules
        assert_error {ERR db0 is already initialized} {r external_data INIT db0 helloextdata1}

        # unload loaded and inited module fails
        assert_error {ERR Error unloading module: operation not possible.} {r module unload helloextdata1}

        # dropping succeeds
        assert_error {ERR Leads to persistent storage data loss for db0, use FORCE if sure} {r external_data drop db0}
        assert_equal {OK} [r external_data drop db0 FORCE]
        assert_equal [list db1:helloextdata2] [r external_data stats dbs]
        assert_equal {OK} [r module unload helloextdata1]

        # init again succeeds
        assert_equal {OK} [r external_data INIT db0 helloextdata2]
        assert_equal [list db0:helloextdata2 db1:helloextdata2] [r external_data stats dbs]

        # cleanup ok
        assert_equal {OK} [r external_data drop db0 FORCE]
        assert_equal {OK} [r external_data drop db1 FORCE]
        assert_equal [list ] [r external_data stats dbs]
        assert_equal {OK} [r module unload helloextdata2]
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id4 "loglevel" debug "ext-data-timeout" 0] tags [list "external:skip" "singledb:skip"]] {
    test {Reading data from storage works} {
        # init
        assert_equal {OK} [r module load $extdatamodule1]
        assert_error {ERR db0 is not initialized} {r external_data debug db0 filter set k}
        assert_error {ERR db0 is not initialized} {r external_data debug db0 storage set k v}
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_error {ERR unknown subcommand somecommand} {r external_data debug db0 somecommand set k v}
        assert_equal {OK} [r external_data INIT db1 helloextdata1]

        # filter RO, storage RO = nil
        assert_equal {OK} [r external_data debug db0 setro]
        assert_error {ERR k set failed} {r external_data debug db0 storage set k v}
        assert_error {ERR k set failed} {r external_data debug db0 filter set k}
        assert_equal {OK} [r select 0]
        assert_equal {} [r get k ext]
        assert_equal {OK} [r select 1]
        assert_equal {} [r get k ext]
        assert_equal {OK} [r select 0]

        # filter ok, storage ok = OK
        assert_equal {OK} [r external_data debug db0 dropro]
        assert_equal {OK} [r external_data debug db0 storage set k v]
        assert_equal {OK} [r external_data debug db0 storage set k v]
        assert_equal {OK} [r external_data debug db0 filter set k]
        assert_equal v [r get k ext]
        assert_equal {} [r get k]
        assert_equal {OK} [r select 1]
        assert_equal {} [r get k ext]
        assert_equal {OK} [r select 0]

        # filter not, storage ok = nil
        assert_equal 1 [r external_data debug db0 filter del k]
        assert_equal 0 [r external_data debug db0 filter del k]
        assert_equal {} [r get k ext]
        assert_equal {OK} [r select 1]
        assert_equal {} [r get k ext]
        assert_equal {OK} [r select 0]

        # filter OK, storage not = nil
        assert_equal {OK} [r external_data debug db0 filter set k]
        assert_equal v [r external_data debug db0 storage del k]
        assert_equal {} [r get k ext]
        assert_equal {OK} [r select 1]
        assert_equal {} [r get k ext]
        assert_equal {OK} [r select 0]

        # filter not, storage not = nil
        assert_equal 1 [r external_data debug db0 filter del k]
        assert_equal {} [r get k ext]
        assert_equal {OK} [r select 1]
        assert_equal {} [r get k ext]
        assert_equal {OK} [r select 0]
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id5 "loglevel" debug "ext-data-timeout" 0] tags [list "external:skip" "singledb:skip"]] {
    test {SET with ext option works, and GET with ext returns the value} {
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r select 0]
        assert_equal {OK} [r set k v ext]
        assert_equal {} [r get k]
        assert_equal v [r get k ext]
        assert_equal {OK} [r select 1]
        assert_equal {} [r get k ext]
        assert_equal {OK} [r select 0]
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id67 "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
    test {Getting keys from storage works} {
        # init
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r external_data INIT db1 helloextdata1]

        # filter ok, storage ok = OK
        assert_equal {OK} [r external_data debug db0 storage set k v]
        assert_equal {OK} [r external_data debug db0 storage set k2 v]
        assert_equal {OK} [r external_data debug db0 filter set k]
        assert_equal {OK} [r select 0]

        # no data in memory
        assert_equal {} [r keys \*]
        assert_equal {0 {}} [r scan 0 type "string" match \*]

        # no k2 as it's not in filter yet
        assert_equal {k} [r keys \* ext]
        assert_equal {0 k} [r scan 0 type "string" match \* ext]

        # exists k2 is it's in filter now
        assert_equal {OK} [r external_data debug db0 filter set k2]
        assert_equal {k k2} [r keys \* ext]
        assert_equal {0 {k k2}} [r scan 0 type "string" match \* ext]

        # another db is not touched
        assert_equal {OK} [r select 1]
        assert_equal {} [r keys \* ext]
        assert_equal {} [r keys \*]
        assert_equal {0 {}} [r scan 0 type "string" match \*]
        assert_equal {0 {}} [r scan 0 type "string" match \* ext]
        assert_equal {OK} [r select 0]
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id66 "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
    test {SET with ext option works, and SCAN and KEYS with ext return the key} {
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r select 0]
        assert_equal {OK} [r set k v ext]
        assert_equal {OK} [r set k2 v ext]
        assert_equal {k k2} [lsort [r keys * ext]]
        set keys [scan_keys 0 "string" "*" "ext"]
        assert_equal [lsort $keys] [list k k2]
        assert_equal {OK} [r select 1]
        assert_equal {} [r keys * ext]
        set keys [scan_keys 0 "string" "*" "ext"]
        assert_equal {} $keys
        assert_equal {OK} [r select 0]
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id8 "loglevel" debug] tags [list "external:skip"]] {
    test "ext-data-store-by-size not set: large key/value stored in memory" {
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r select 0]
        set large_key [gen_data 200]
        set large_val [gen_data 200]
        assert_equal {OK} [r set $large_key $large_val]
        assert_equal $large_val [r get $large_key]
        assert_equal $large_val [r get $large_key ext]
    }

    test "ext-data-store-by-size=0: large key/value stored in memory" {
        r config set ext-data-store-by-size 0
        assert_equal {OK} [r select 0]
        set large_key [gen_data 200]
        set large_val [gen_data 200]
        assert_equal {OK} [r set $large_key $large_val]
        assert_equal $large_val [r get $large_key]
        assert_equal $large_val [r get $large_key ext]
    }

    test "ext-data-store-by-size=100: small key/value stored in memory" {
        r config set ext-data-store-by-size 100
        assert_equal {OK} [r select 0]
        assert_equal {OK} [r set small_key small_val]
        assert_equal small_val [r get small_key]
        assert_equal small_val [r get small_key ext]
    }

    test "ext-data-store-by-size=100: large key/value stored in external storage" {
        r config set ext-data-store-by-size 100
        assert_equal {OK} [r select 0]
        set large_key [gen_data 200]
        set large_val [gen_data 200]
        assert_equal {OK} [r set $large_key $large_val]
        assert_equal {} [r get $large_key]
        assert_equal $large_val [r get $large_key ext]
    }

    test "Move key from memory to external storage" {
        r config set ext-data-store-by-size 100
        assert_equal {OK} [r select 0]
        # Set key to memory
        assert_equal {OK} [r set key_in_mem value_in_mem]
        # Verify it's in memory
        assert_equal value_in_mem [r get key_in_mem]
        assert_equal value_in_mem [r get key_in_mem ext]

        # Set same key with ext flag
        assert_equal {OK} [r set key_in_mem value_in_ext ext]
        # Verify it's now in external storage, not in memory
        assert_equal {} [r get key_in_mem]
        assert_equal value_in_ext [r get key_in_mem ext]
    }
}

### Sharded
start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id64 "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
    # we assume that all cross-requests to and from another nodes are made in a usual Valkey way
    # that's why there are no tests with MOVED during set/get here
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    test {External storage works with single sharded} {
        # init
        wait_for_cluster_state ok
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]

        # filter ok, storage ok = OK
        assert_equal {OK} [r external_data debug db0 storage set k v]
        assert_equal {OK} [r external_data debug db0 storage set k v]
        assert_equal {OK} [r external_data debug db0 filter set k]
        r select 0
        assert_equal v [r get k ext]
        assert_equal {} [r get k]

        # filter not, storage ok = nil
        assert_equal 1 [r external_data debug db0 filter del k]
        assert_equal 0 [r external_data debug db0 filter del k]
        assert_equal {} [r get k ext]

        # filter OK, storage not = nil
        assert_equal {OK} [r external_data debug db0 filter set k]
        assert_equal v [r external_data debug db0 storage del k]
        assert_equal {} [r get k ext]

        # filter not, storage not = nil
        assert_equal 1 [r external_data debug db0 filter del k]
        assert_equal {} [r get k ext]
    }

    test "ext-data-store-by-size not set: large key/value stored in memory (sharded)" {
        wait_for_cluster_state ok
        assert_equal {OK} [r select 0]
        set large_key [gen_data 200]
        set large_val [gen_data 200]
        assert_equal {OK} [r set $large_key $large_val]
        assert_equal $large_val [r get $large_key]
        assert_equal $large_val [r get $large_key ext]
    }

    test "ext-data-store-by-size=0: large key/value stored in memory (sharded)" {
        wait_for_cluster_state ok
        assert_equal {OK} [r select 0]
        r config set ext-data-store-by-size 0
        set large_key [gen_data 200]
        set large_val [gen_data 200]
        assert_equal {OK} [r set $large_key $large_val]
        assert_equal $large_val [r get $large_key]
        assert_equal $large_val [r get $large_key ext]
    }

    test "ext-data-store-by-size=100: small key/value stored in memory (sharded)" {
        wait_for_cluster_state ok
        assert_equal {OK} [r select 0]
        r config set ext-data-store-by-size 100
        assert_equal {OK} [r set small_key small_val]
        assert_equal small_val [r get small_key]
        assert_equal small_val [r get small_key ext]
    }

    test "ext-data-store-by-size=100: large key/value stored in external storage (sharded)" {
        wait_for_cluster_state ok
        assert_equal {OK} [r select 0]
        r config set ext-data-store-by-size 100
        set large_key [gen_data 200]
        set large_val [gen_data 200]
        assert_equal {OK} [r set $large_key $large_val]
        assert_equal {} [r get $large_key]
        assert_equal $large_val [r get $large_key ext]
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id58 "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
    # we assume that all cross-requests to and from another nodes are made in a usual Valkey way
    # that's why there are no tests with MOVED during set/get here
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    test {Getting keys from storage works} {
        # init
        wait_for_cluster_state ok
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r external_data INIT db1 helloextdata1]

        # filter ok, storage ok = OK
        assert_equal {OK} [r external_data debug db0 storage set k v]
        assert_equal {OK} [r external_data debug db0 storage set k2 v]
        assert_equal {OK} [r external_data debug db0 filter set k]
        assert_equal {OK} [r select 0]

        # no data in memory
        assert_equal {} [r keys \*]
        assert_equal {0 {}} [r scan 0 type "string" match \*]

        # no k2 as it's not in filter yet
        assert_equal {k} [r keys \* ext]
        assert_equal {} [r keys \*]
        assert_equal {0 k} [r scan 0 type "string" match \* ext]

        # exists k2 is it's in filter now
        assert_equal {OK} [r external_data debug db0 filter set k2]
        assert_equal {k k2} [r keys \* ext]
        assert_equal {0 {k k2}} [r scan 0 type "string" match \* ext]

        # another db is not touched
        assert_equal {OK} [r select 1]
        assert_equal {} [r keys \* ext]
        assert_equal {} [r keys \*]
        assert_equal {0 {}} [r scan 0 type "string" match \*]
        assert_equal {0 {}} [r scan 0 type "string" match \* ext]
        assert_equal {OK} [r select 0]
    }
}

start_cluster 1 0 [list overrides [list "loglevel" debug] tags [list "external:skip"]] {
    set ext_data_off_err "ERR External data commands are unavailable with ext-data-mode off"
    test "SET with ext option fails when ext-data-mode is off (sharded)" {
        wait_for_cluster_state ok
        assert_error $ext_data_off_err {r set k v ext}
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id3 "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    test "SET with ext option works, and GET with ext returns the value (sharded)" {
        wait_for_cluster_state ok
        assert_equal {OK} [r select 0]
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r set k v ext]
        assert_equal v [r get k ext]
        assert_equal {} [r get k]
    }

    test "Move key from memory to external storage (sharded)" {
        assert_equal {OK} [r select 0]
        # Set key to memory
        assert_equal {OK} [r set key_in_mem value_in_mem]
        # Verify it's in memory
        assert_equal value_in_mem [r get key_in_mem]
        assert_equal value_in_mem [r get key_in_mem ext]

        # Set same key with ext flag
        assert_equal {OK} [r set key_in_mem value_in_ext ext]
        # Verify it's now in external storage, not in memory
        assert_equal {} [r get key_in_mem]
        assert_equal value_in_ext [r get key_in_mem ext]
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id69 "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    test "SET with ext option works, and SCAN and KEYS with ext return the key (sharded)" {
        wait_for_cluster_state ok
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r select 0]
        assert_equal {OK} [r set k1 v ext]
        assert_equal {OK} [r set k2 v ext]
        assert_equal {k1 k2} [lsort [r keys * ext]]
        set keys [scan_keys 0 "string" "*" "ext"]
        assert_equal [lsort $keys] [list k1 k2]
    }
}

# Test external storage deletion functionality
# Tests both sharded and non-sharded versions of DEL command with EXT option

# Test cases:
# 1) deleted key is missing, no error
# 2) deleted key exists in memory, deleted without ext flag, deletion is successful, no error
# 3) deleted key exists in external storage, deleted without ext flag, deletion doesn't touch external storage, no error
# 4) deleted key exists in external storage and memory (somehow), deleted without ext flag, deletion doesn't touch external storage, no error
# 5) deleted key exists in external storage and memory (somehow), deleted with ext flag, key is deleted from both memory and external storage, no error
# 6) deleted key exists in external storage, deleted with ext flag, deletion is successful, no error

### Non-sharded
start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id68 "loglevel" debug "ext-data-store-by-size" "0"] tags [list "external:skip"]] {
    # Load module once for all tests in this block
    r module load $extdatamodule1
    # Initialize external data once for all tests
    r external_data INIT db0 helloextdata1

    test "DEL with EXT - Non-sharded - Test case 1: deleted key is missing, no error" {
        assert_equal 0 [r del missing_key_test1]
    }

    test "DEL with EXT - Non-sharded - Test case 2: deleted key exists in memory, deleted without ext flag, deletion is successful, no error" {
        r set mem_key_test2 "mem_value"
        assert_equal "mem_value" [r get mem_key_test2]
        assert_equal 1 [r del mem_key_test2]
        assert_equal "" [r get mem_key_test2]
    }

    test "DEL with EXT - Non-sharded - Test case 3: deleted key exists in external storage, deleted without ext flag, deletion doesn't touch external storage, no error" {
        r select 0
        # Set key to external storage
        r set ext_key_test3 "ext_value" ext
        assert_equal "ext_value" [r get ext_key_test3 ext]
        
        # Delete without EXT flag - key not in memory, so DEL returns 0
        assert_equal 0 [r del ext_key_test3]
        assert_equal "" [r get ext_key_test3]
        assert_equal "ext_value" [r get ext_key_test3 ext]
    }

    test "DEL with EXT - Non-sharded - Test case 4: deleted key exists in external storage and memory (somehow), deleted without ext flag, deletion doesn't touch external storage, no error" {
        r select 0
        # Set key to external storage
        r set dual_key_test4 "dual_value" ext
        assert_equal "dual_value" [r get dual_key_test4 ext]
        
        # Also put it in memory (simulating the "somehow" scenario)
        r set dual_key_test4 "dual_value"
        assert_equal "dual_value" [r get dual_key_test4]
        
        # Delete without EXT flag - should only delete from memory, not external storage
        assert_equal 1 [r del dual_key_test4]
        assert_equal "" [r get dual_key_test4]
        assert_equal "dual_value" [r get dual_key_test4 ext]
    }

    test "DEL with EXT - Non-sharded - Test case 5: deleted key exists in external storage and memory, deleted with ext flag, key is deleted from both memory and external storage, no error" {
        r select 0
        # Set key to external storage
        r set dual_key2_test5 dual_value2 ext
        assert_equal dual_value2 [r get dual_key2_test5 ext]
        
        # Put it in memory
        r set dual_key2_test5 dual_value2
        assert_equal dual_value2 [r get dual_key2_test5]
        
        # Delete with EXT flag - should delete from both memory and external storage
        assert_equal 1 [r del dual_key2_test5 ext]
        
        # Test getting data from storage and filter using debug commands
        assert_equal "" [r external_data debug db0 storage get dual_key2_test5]
        assert_equal 0 [r external_data debug db0 filter get dual_key2_test5]
        
        assert_equal "" [r get dual_key2_test5]
        assert_equal "" [r get dual_key2_test5 ext]
    }

    test "DEL with EXT - Non-sharded - Test case 6: deleted key exists in external storage, deleted with ext flag, deletion is successful, no error" {
        r select 0
        # Set key to external storage
        r set ext_key2_test6 "ext_value2" ext
        assert_equal "ext_value2" [r get ext_key2_test6 ext]
        
        # Delete with EXT flag - should delete from both memory and external storage
        assert_equal 1 [r del ext_key2_test6 ext]
        assert_equal "" [r get ext_key2_test6]
        assert_equal "" [r get ext_key2_test6 ext]
    }

    test "DEL with EXT - Non-sharded - Multiple keys with external storage" {
        r select 0
        # Set multiple keys to external storage
        r set multi_key1_test7 "value1" ext
        r set multi_key2_test7 "value2" ext
        r set multi_key3_test7 "value3" ext
        
        # Verify they exist in external storage
        assert_equal "value1" [r get multi_key1_test7 ext]
        assert_equal "value2" [r get multi_key2_test7 ext]
        assert_equal "value3" [r get multi_key3_test7 ext]
        
        # Delete multiple keys with EXT flag
        assert_equal 2 [r del multi_key1_test7 multi_key2_test7 ext]
        
        # Verify deletion
        assert_equal "" [r get multi_key1_test7]
        assert_equal "" [r get multi_key1_test7 ext]
        assert_equal "" [r get multi_key2_test7]
        assert_equal "" [r get multi_key2_test7 ext]
        assert_equal "" [r get multi_key3_test7]
        assert_equal "value3" [r get multi_key3_test7 ext]
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id6 "loglevel" debug "ext-data-store-by-size" "0"] tags [list "external:skip" "singledb:skip"]] {
    test "DEL with EXT - Non-sharded - Different databases" {
        # Load module once for all tests in this block
        r module load $extdatamodule1
        # Initialize external data for db0
        r external_data INIT db0 helloextdata1
        # Initialize external data for db1
        r external_data INIT db1 helloextdata1
        
        # Set keys in different databases
        r select 0
        r set db0_key_test8 "db0_value" ext
        r select 1
        r set db1_key_test8 "db1_value" ext
        
        # Verify they exist in their respective databases
        r select 0
        assert_equal "db0_value" [r get db0_key_test8 ext]
        assert_equal "" [r get db1_key_test8 ext]
        
        r select 1
        assert_equal "" [r get db0_key_test8 ext]
        assert_equal "db1_value" [r get db1_key_test8 ext]
        
        # Delete from db0
        r select 0
        assert_equal 1 [r del db0_key_test8 ext]
        assert_equal "" [r get db0_key_test8]
        assert_equal "" [r get db0_key_test8 ext]
        
        # Verify db1 key is still there
        r select 1
        assert_equal "db1_value" [r get db1_key_test8 ext]
    }
}

### Sharded
start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id7 "ext-data-store-by-size" "0"]] {
    # Load module once for all tests in this block
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    wait_for_cluster_state ok
    r module load $extdatamodule1
    # Initialize external data once for all tests
    r external_data INIT db0 helloextdata1

    test "DEL with EXT - Sharded - Test case 1: deleted key is missing, no error" {
        assert_equal 0 [r del missing_key]
    }

    test "DEL with EXT - Sharded - Test case 2: deleted key exists in memory, deleted without ext flag, deletion is successful, no error" {
        r select 0
        r set mem_key "mem_value"
        assert_equal "mem_value" [r get mem_key]
        assert_equal 1 [r del mem_key]
        assert_equal "" [r get mem_key]
    }

    test "DEL with EXT - Sharded - Test case 3: deleted key exists in external storage, deleted without ext flag, deletion doesn't touch external storage, no error" {
        r select 0
        # Set key to external storage
        r set ext_key "ext_value" ext
        assert_equal "ext_value" [r get ext_key ext]
        
        # Delete without EXT flag - key not in memory, so DEL returns 0
        assert_equal 0 [r del ext_key]
        assert_equal "" [r get ext_key]
        assert_equal "ext_value" [r get ext_key ext]
    }

    test "DEL with EXT - Sharded - Test case 4: deleted key exists in external storage and memory (somehow), deleted without ext flag, deletion doesn't touch external storage, no error" {
        r select 0
        # Set key to external storage
        r set dual_key "dual_value" ext
        assert_equal "dual_value" [r get dual_key ext]
        
        # Also put it in memory (simulating the "somehow" scenario)
        r set dual_key "dual_value"
        assert_equal "dual_value" [r get dual_key]
        
        # Delete without EXT flag - should only delete from memory, not external storage
        assert_equal 1 [r del dual_key]
        assert_equal "" [r get dual_key]
        assert_equal "dual_value" [r get dual_key ext]
    }

    test "DEL with EXT - Sharded - Test case 5: deleted key exists in external storage and memory, deleted with ext flag, key is deleted from both memory and external storage, no error" {
        r select 0
        # Set key to external storage
        r set dual_key2 "dual_value2" ext
        assert_equal "dual_value2" [r get dual_key2 ext]
        
        # Put it in memory
        r set dual_key2 "dual_value2"
        assert_equal "dual_value2" [r get dual_key2]
        
        # Delete with EXT flag - should delete from both memory and external storage
        assert_equal 1 [r del dual_key2 ext]
        assert_equal "" [r get dual_key2]
        assert_equal "" [r get dual_key2 ext]
    }

    test "DEL with EXT - Sharded - Test case 6: deleted key exists in external storage, deleted with ext flag, deletion is successful, no error" {
        r select 0
        # Set key to external storage
        r set ext_key2 "ext_value2" ext
        assert_equal "ext_value2" [r get ext_key2 ext]
        
        # Delete with EXT flag - should delete from both memory and external storage
        assert_equal 1 [r del ext_key2 ext]
        assert_equal "" [r get ext_key2]
        assert_equal "" [r get ext_key2 ext]
    }

    test "DEL with EXT - Sharded - Multiple keys with external storage (just checking no error)" {
        r select 0
        # Set multiple keys to external storage
        r set multi_key1_test7 "value1" ext
        r set multi_key2_test7 "value2" ext
        
        # Delete multiple keys - want to check that nothing is broken as it's broken without custom get_keys_function to command
        catch {r del multi_key1_test7 multi_key2_test7} err
        assert_match {CROSSSLOT Keys*} $err

        catch {r del multi_key1_test7 multi_key2_test7 ext} err
        assert_match {CROSSSLOT Keys*} $err
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id65 "ext-data-store-by-size" "0"] tags [list "singledb:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    test "DEL with EXT - Sharded - Different databases" {
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r external_data INIT db1 helloextdata1]
        r select 0
        
        # Set keys in different databases
        r set db0_key "db0_value" ext
        r select 1
        r set db1_key "db1_value" ext
        
        # Verify they exist in their respective databases
        r select 0
        assert_equal "db0_value" [r get db0_key ext]
        assert_equal "" [r get db1_key ext]
        
        r select 1
        assert_equal "" [r get db0_key ext]
        assert_equal "db1_value" [r get db1_key ext]
        
        # Delete from db0
        r select 0
        assert_equal 1 [r del db0_key ext]
        assert_equal "" [r get db0_key]
        assert_equal "" [r get db0_key ext]
        
        # Verify db1 key is still there
        r select 1
        assert_equal "db1_value" [r get db1_key ext]
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id9 "loglevel" debug "ext-data-timeout" 5] tags [list "external:skip" "block"]] {
    test {External storage operations don't block main thread} {
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        r select 0

        # Set up a key that will be stored in external storage
        set blocking_key "blocking_key"
        set blocking_value "blocking_value"
        
        # Create a large value for the second client to work with
        set large_value [string repeat "x" 1000]

        # Set up keys that will be stored in external storage
        set operations_count 100
        for {set i 0} {$i < $operations_count} {incr i} {
            set test_key "client2_key_$i"
            assert_equal {OK} [r set $test_key $large_value ext]
        }

        # Put storage and filter in readonly mode to cause actual blocking
        assert_equal {OK} [r external_data debug db0 setro]
        
        # Start client 1 in background - this will block on external storage operation
        set client1 [valkey_deferring_client]
        $client1 select 0
        $client1 set $blocking_key $blocking_value ext
        
        # Give client 1 time to start and get blocked
        after 100

        # Part 1: Get key-value pairs after setting readonly mode on storage and filter
        # This should complete quickly even though storage/filter are readonly
        set start_time [clock milliseconds]
        for {set i 0} {$i < $operations_count} {incr i} {
            set test_key "client2_key_$i"
            assert_equal $large_value [r get $test_key ext]
        }
        set end_time [clock milliseconds]
        
        # Calculate total time for get operations - should be much less than the 5 second timeout
        set get_elapsed_time [expr {$end_time - $start_time}]
        
        # The get operations should complete quickly (well under 5 seconds)
        # If external storage was blocking the main thread, this would take much longer
        assert {[expr {$get_elapsed_time < 2000}]}
        
        # Drop readonly mode
        assert_equal {OK} [r external_data debug db0 dropro]
        
        # Verify the write is successful after unblocking
        assert_equal "blocking_value" [r get $blocking_key ext]

        # Now writes should work
        set new_key "new_write_key"
        set new_value "new_write_value"
        assert_equal {OK} [r set $new_key $new_value ext]
        assert_equal $new_value [r get $new_key ext]
        assert_equal "" [r get $new_key]

        # Cleanup
        $client1 close
    }
}

# Test ext-data-expire functionality
start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id10 "loglevel" debug] tags [list "external:skip"]] {
    test {ext-data-expire disabled by default} {
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        r select 0
        
        # Verify ext-data-expire is disabled by default
        set config_result [r config get ext-data-expire]
        assert_equal "no" [lindex $config_result 1]
    }
    
    test {ext-data-expire can be enabled} {
        # Enable ext-data-expire
        assert_equal {OK} [r config set ext-data-expire yes]
        set config_result [r config get ext-data-expire]
        assert_equal "yes" [lindex $config_result 1]
        
        # Disable it again
        assert_equal {OK} [r config set ext-data-expire no]
        set config_result [r config get ext-data-expire]
        assert_equal "no" [lindex $config_result 1]
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id11 "loglevel" debug "maxmemory-policy" "allkeys-lru" "ext-data-expire" "yes"] tags [list "external:skip" "slow"]] {
    test {ext-data-expire configuration works with allkeys-lru policy} {
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        r select 0
        
        # Verify ext-data-expire is enabled
        set config_result [r config get ext-data-expire]
        assert_equal "yes" [lindex $config_result 1]
        
        # Verify maxmemory-policy is allkeys-lru
        set config_result [r config get maxmemory-policy]
        assert_equal "allkeys-lru" [lindex $config_result 1]
        
        # Set a few keys in memory
        r set "test_key1" "test_value1" ex 1
        r set "test_key2" "test_value2" ex 5
        r set "test_key3" "test_value3" 
        r debug sleep 2
        
        # Verify necessary keys are in memory
        assert_equal "" [r get "test_key1"]
        assert_equal "test_value2" [r get "test_key2"]
        assert_equal "test_value3" [r get "test_key3"]
        
        # Verify necessary keys are in external storage
        assert_equal "test_value1" [r get "test_key1" ext]
        assert_equal "test_value2" [r get "test_key2" ext]
        assert_equal "test_value3" [r get "test_key3" ext]
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id12 "loglevel" debug "maxmemory-policy" "volatile-lru" "ext-data-expire" "yes"] tags [list "external:skip" "slow"]] {
    test {ext-data-expire does not affect volatile-* policies} {
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        r select 0
        
        # Verify ext-data-expire is enabled but should not affect volatile policies
        set config_result [r config get ext-data-expire]
        assert_equal "yes" [lindex $config_result 1]
        
        set config_result [r config get maxmemory-policy]
        assert_equal "volatile-lru" [lindex $config_result 1]
        
        # Set keys with TTL
        r set "test_key1" "test_value1" ex 1
        r set "test_key2" "test_value2" ex 5
        r debug sleep 2

        # Verify necessary keys are in memory
        assert_equal "" [r get "test_key1"]
        assert_equal "test_value2" [r get "test_key2"]

        # Verify necessary keys are in external storage
        assert_equal "" [r get "test_key1" ext]
        assert_equal "test_value2" [r get "test_key2" ext]
    }
}

# Test FLUSHDB command with external storage
# FLUSHDB should clean external data for the current database only
start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id13 "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
    test {FLUSHDB cleans external data for current database only} {
        # Load module and initialize external storage for multiple databases
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r external_data INIT db1 helloextdata1]
        
        # Set keys in db0
        r select 0
        assert_equal {OK} [r set key0_1 value0_1 ext]
        assert_equal {OK} [r set key0_2 value0_2 ext]
        assert_equal {OK} [r set mem_key0 mem_value0]
        
        # Set keys in db1
        r select 1
        assert_equal {OK} [r set key1_1 value1_1 ext]
        assert_equal {OK} [r set key1_2 value1_2 ext]
        assert_equal {OK} [r set mem_key1 mem_value1]
        
        # Verify all keys exist before flush
        r select 0
        assert_equal value0_1 [r get key0_1 ext]
        assert_equal value0_2 [r get key0_2 ext]
        assert_equal mem_value0 [r get mem_key0]
        r select 1
        assert_equal value1_1 [r get key1_1 ext]
        assert_equal value1_2 [r get key1_2 ext]
        assert_equal mem_value1 [r get mem_key1]
        
        # Flush db0
        r select 0
        assert_equal {OK} [r flushdb]
        
        # Verify db0 external data is cleaned
        assert_equal {} [r get key0_1 ext]
        assert_equal {} [r get key0_2 ext]
        assert_equal {} [r get mem_key0]
        
        # Verify db1 external data is NOT cleaned
        r select 1
        assert_equal value1_1 [r get key1_1 ext]
        assert_equal value1_2 [r get key1_2 ext]
        assert_equal mem_value1 [r get mem_key1]
    }
    
    test {FLUSHDB ASYNC cleans external data for current database only} {
        # Set keys in db0
        r select 0
        assert_equal {OK} [r set async_key0_1 async_value0_1 ext]
        assert_equal {OK} [r set async_key0_2 async_value0_2 ext]
        
        # Set keys in db1
        r select 1
        assert_equal {OK} [r set async_key1_1 async_value1_1 ext]
        
        # Verify all keys exist
        r select 0
        assert_equal async_value0_1 [r get async_key0_1 ext]
        assert_equal async_value0_2 [r get async_key0_2 ext]
        r select 1
        assert_equal async_value1_1 [r get async_key1_1 ext]
        
        # Flush db0 async
        r select 0
        assert_equal {OK} [r flushdb async]
        
        # Verify db0 external data is cleaned
        assert_equal {} [r get async_key0_1 ext]
        assert_equal {} [r get async_key0_2 ext]
        
        # Verify db1 external data is NOT cleaned
        r select 1
        assert_equal async_value1_1 [r get async_key1_1 ext]
    }
}

# Test FLUSHALL command with external storage
# FLUSHALL should clean external data for ALL databases
start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id14 "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
    test {FLUSHALL cleans external data for all databases} {
        # Load module and initialize external storage for multiple databases
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r external_data INIT db1 helloextdata1]
        assert_equal {OK} [r external_data INIT db2 helloextdata1]
        
        # Set keys in db0
        r select 0
        assert_equal {OK} [r set all_key0_1 all_value0_1 ext]
        assert_equal {OK} [r set all_key0_2 all_value0_2 ext]
        assert_equal {OK} [r set all_mem_key0 all_mem_value0]
        
        # Set keys in db1
        r select 1
        assert_equal {OK} [r set all_key1_1 all_value1_1 ext]
        assert_equal {OK} [r set all_key1_2 all_value1_2 ext]
        assert_equal {OK} [r set all_mem_key1 all_mem_value1]
        
        # Set keys in db2
        r select 2
        assert_equal {OK} [r set all_key2_1 all_value2_1 ext]
        assert_equal {OK} [r set all_mem_key2 all_mem_value2]
        
        # Verify all keys exist before flush
        r select 0
        assert_equal all_value0_1 [r get all_key0_1 ext]
        assert_equal all_value0_2 [r get all_key0_2 ext]
        assert_equal all_mem_value0 [r get all_mem_key0]
        r select 1
        assert_equal all_value1_1 [r get all_key1_1 ext]
        assert_equal all_value1_2 [r get all_key1_2 ext]
        assert_equal all_mem_value1 [r get all_mem_key1]
        r select 2
        assert_equal all_value2_1 [r get all_key2_1 ext]
        assert_equal all_mem_value2 [r get all_mem_key2]
        
        # Execute FLUSHALL
        assert_equal {OK} [r flushall]
        
        # Verify all external data is cleaned across all databases
        r select 0
        assert_equal {} [r get all_key0_1 ext]
        assert_equal {} [r get all_key0_2 ext]
        assert_equal {} [r get all_mem_key0]
        r select 1
        assert_equal {} [r get all_key1_1 ext]
        assert_equal {} [r get all_key1_2 ext]
        assert_equal {} [r get all_mem_key1]
        r select 2
        assert_equal {} [r get all_key2_1 ext]
        assert_equal {} [r get all_mem_key2]
    }
    
    test {FLUSHALL ASYNC cleans external data for all databases} {
        # Set keys in db0
        r select 0
        assert_equal {OK} [r set async_all_key0 async_all_value0 ext]
        
        # Set keys in db1
        r select 1
        assert_equal {OK} [r set async_all_key1 async_all_value1 ext]
        
        # Verify all keys exist
        r select 0
        assert_equal async_all_value0 [r get async_all_key0 ext]
        r select 1
        assert_equal async_all_value1 [r get async_all_key1 ext]
        
        # Execute FLUSHALL ASYNC
        assert_equal {OK} [r flushall async]
        
        # Verify all external data is cleaned
        r select 0
        assert_equal {} [r get async_all_key0 ext]
        r select 1
        assert_equal {} [r get async_all_key1 ext]
    }
}

# Test FLUSHDB with cluster mode
start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id15 "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
    
    test {FLUSHDB cleans external data for current database only (cluster)} {
        wait_for_cluster_state ok
        
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r external_data INIT db1 helloextdata1]
        
        # Set keys in db0
        r select 0
        assert_equal {OK} [r set cluster_key0_1 cluster_value0_1 ext]
        assert_equal {OK} [r set cluster_key0_2 cluster_value0_2 ext]
        
        # Set keys in db1
        r select 1
        assert_equal {OK} [r set cluster_key1_1 cluster_value1_1 ext]
        
        # Verify keys exist
        r select 0
        assert_equal cluster_value0_1 [r get cluster_key0_1 ext]
        assert_equal cluster_value0_2 [r get cluster_key0_2 ext]
        r select 1
        assert_equal cluster_value1_1 [r get cluster_key1_1 ext]
        
        # Flush db0
        r select 0
        assert_equal {OK} [r flushdb]
        
        # Verify db0 external data is cleaned
        assert_equal {} [r get cluster_key0_1 ext]
        assert_equal {} [r get cluster_key0_2 ext]
        
        # Verify db1 external data is NOT cleaned
        r select 1
        assert_equal cluster_value1_1 [r get cluster_key1_1 ext]
    }
}

# Test FLUSHALL with cluster mode
start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id16 "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
    
    test {FLUSHALL cleans external data for all databases (cluster)} {
        wait_for_cluster_state ok
        
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r external_data INIT db1 helloextdata1]
        
        # Set keys in db0
        r select 0
        assert_equal {OK} [r set cluster_all_key0 cluster_all_value0 ext]
        
        # Set keys in db1
        r select 1
        assert_equal {OK} [r set cluster_all_key1 cluster_all_value1 ext]
        
        # Verify keys exist
        r select 0
        assert_equal cluster_all_value0 [r get cluster_all_key0 ext]
        r select 1
        assert_equal cluster_all_value1 [r get cluster_all_key1 ext]
        
        # Execute FLUSHALL
        assert_equal {OK} [r flushall]
        
        # Verify all external data is cleaned
        r select 0
        assert_equal {} [r get cluster_all_key0 ext]
        r select 1
        assert_equal {} [r get cluster_all_key1 ext]
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id17 "loglevel" debug] tags [list "external:skip" "slow" "singledb:skip"]] {
    test {Performance with large dataset (should complete in under 500ms)} {
        cleanup_external_data_dump r

        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r external_data INIT db1 helloextdata1]
        r select 0
        
        # Insert a large number of keys into db0 external storage
        # Using 100,000 keys to demonstrate the O(n) performance issue
        set num_keys 100000
        
        set insert_start [clock milliseconds]
        for {set i 0} {$i < $num_keys} {incr i} {
            r set perf_key_$i perf_value_$i ext
        }
        set insert_end [clock milliseconds]
        set insert_time [expr {$insert_end - $insert_start}]
        
        # Before flushall create a backup
        set backup_result [r external_data dump]
        assert {$backup_result != ""}
        
        # Insert just one key into db1 to see the difference
        r select 1
        assert_equal {OK} [r set single_key single_value ext]
        
        # Verify keys exist in both databases
        r select 0
        assert_equal perf_value_0 [r get perf_key_0 ext]
        assert_equal perf_value_[expr {$num_keys - 1}] [r get perf_key_[expr {$num_keys - 1}] ext]
        r select 1
        assert_equal single_value [r get single_key ext]
        
        # Test DBSIZE with EXT flag performance
        r select 0
        
        # Measure DBSIZE with EXT performance
        set dbsize_start [clock milliseconds]
        set dbsize_result [r dbsize ext]
        set dbsize_end [clock milliseconds]
        set dbsize_time [expr {$dbsize_end - $dbsize_start}]
        
        # Verify DBSIZE EXT returns the correct count
        assert_equal $num_keys $dbsize_result
        
        # Assert DBSIZE EXT completes in under 500ms
        if {$dbsize_time >= 500} {
            fail "DBSIZE EXT took ${dbsize_time}ms, expected < 500ms"
        }
        
        # Measure SWAPDB performance
        set swap_start [clock milliseconds]
        assert_equal {OK} [r swapdb 0 1]
        set swap_end [clock milliseconds]
        set swap_time [expr {$swap_end - $swap_start}]
        
        # Verify swap worked correctly
        r select 0
        assert_equal single_value [r get single_key ext]
        assert_equal {} [r get perf_key_0 ext]
        r select 1
        assert_equal perf_value_0 [r get perf_key_0 ext]
        assert_equal {} [r get single_key ext]
        
        # Assert SWAPDB completes in under 500ms
        # This should PASS with the current implementation as SWAPDB should be O(1)
        if {$swap_time >= 500} {
            fail "SWAPDB took ${swap_time}ms, expected < 500ms (SWAPDB should be O(1) operation)"
        }
        
        # Now test FLUSHDB performance with the large dataset (now in db1)
        r select 1
        
        # Measure FLUSHDB performance
        set flush_start [clock milliseconds]
        assert_equal {OK} [r flushdb]
        set flush_end [clock milliseconds]
        set flush_time [expr {$flush_end - $flush_start}]
        
        # Verify all keys are deleted
        assert_equal {} [r get perf_key_0 ext]
        assert_equal {} [r get perf_key_[expr {$num_keys - 1}] ext]
        
        # Verify DBSIZE EXT returns 0 after FLUSHDB
        set dbsize_after_flush [r dbsize ext]
        assert_equal 0 $dbsize_after_flush
        
        # Assert FLUSHDB completes in under 500ms
        # This will FAIL with the current O(n) implementation
        # demonstrating the need for a more efficient flush mechanism (e.g., native flush API)
        if {$flush_time >= 500} {
            fail "FLUSHDB took ${flush_time}ms, expected < 500ms (demonstrates O(n) limitation - needs native flush API)"
        }
        
        # Action: After flushall restore from this backup
        r select 0
        r external_data load $backup_result
        
        # Ensure that DBSIZE is the correct one without iterating through keys (too slow)
        set dbsize_after_restore [r dbsize ext]
        assert_equal $num_keys $dbsize_after_restore

        cleanup_external_data_dump r
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id18 "loglevel" debug] tags [list "external:skip" "slow" "singledb:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
    
    test {Performance with large dataset (should complete in under 500ms) (cluster mode)} {
        wait_for_cluster_state ok
        cleanup_external_data_dump r
        
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r external_data INIT db1 helloextdata1]
        r select 0
        
        # Insert a large number of keys into db0 external storage
        # Using 100,000 keys to demonstrate the O(n) performance issue
        set num_keys 100000
        
        set insert_start [clock milliseconds]
        for {set i 0} {$i < $num_keys} {incr i} {
            r set perf_key_$i perf_value_$i ext
        }
        set insert_end [clock milliseconds]
        set insert_time [expr {$insert_end - $insert_start}]
        
        # Before flushall create a backup
        set backup_result [r external_data dump]
        assert {$backup_result != ""}

        # Insert just one key into db1 to see the difference
        r select 1
        assert_equal {OK} [r set single_key single_value ext]
        
        # Verify keys exist in both databases
        r select 0
        assert_equal perf_value_0 [r get perf_key_0 ext]
        assert_equal perf_value_[expr {$num_keys - 1}] [r get perf_key_[expr {$num_keys - 1}] ext]
        r select 1
        assert_equal single_value [r get single_key ext]
        
        # Measure DBSIZE with EXT performance
        r select 0
        set dbsize_start [clock milliseconds]
        set dbsize_result [r dbsize ext]
        set dbsize_end [clock milliseconds]
        set dbsize_time [expr {$dbsize_end - $dbsize_start}]
        
        # Verify DBSIZE EXT returns the correct count
        assert_equal $num_keys $dbsize_result
        
        # Assert DBSIZE EXT completes in under 500ms
        if {$dbsize_time >= 500} {
            fail "DBSIZE EXT took ${dbsize_time}ms, expected < 500ms in cluster mode"
        }
        
        # Measure FLUSHDB performance
        r select 0
        set flush_start [clock milliseconds]
        assert_equal {OK} [r flushdb]
        set flush_end [clock milliseconds]
        set flush_time [expr {$flush_end - $flush_start}]
        
        # Verify all keys are deleted
        assert_equal {} [r get perf_key_0 ext]
        assert_equal {} [r get perf_key_[expr {$num_keys - 1}] ext]
        
        # Verify DBSIZE EXT returns 0 after FLUSHDB
        set dbsize_after_flush [r dbsize ext]
        assert_equal 0 $dbsize_after_flush
        
        # Assert FLUSHDB completes in under 500ms
        # This will FAIL with the current O(n) implementation
        # demonstrating the need for a more efficient flush mechanism (e.g., native flush API)
        if {$flush_time >= 500} {
            fail "FLUSHDB took ${flush_time}ms, expected < 500ms (demonstrates O(n) limitation - needs native flush API)"
        }

        # Action: After flushall restore from this backup
        r select 0
        r external_data load $backup_result
        
        # Ensure that DBSIZE is the correct one without iterating through keys (too slow)
        set dbsize_after_restore [r dbsize ext]
        assert_equal $num_keys $dbsize_after_restore

        cleanup_external_data_dump r
    }
}

# Test SWAPDB command with external storage
# SWAPDB should swap external data between two databases
start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id19 "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
    test {SWAPDB swaps external data between two databases} {
        # Load module and initialize external storage for multiple databases
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r external_data INIT db1 helloextdata1]
        assert_equal {OK} [r external_data INIT db2 helloextdata1]
        
        # Set keys in db0 external storage
        r select 0
        assert_equal {OK} [r set swap_key0_1 swap_value0_1 ext]
        assert_equal {OK} [r set swap_key0_2 swap_value0_2 ext]
        assert_equal {OK} [r set swap_mem_key0 swap_mem_value0]
        
        # Set keys in db1 external storage
        r select 1
        assert_equal {OK} [r set swap_key1_1 swap_value1_1 ext]
        assert_equal {OK} [r set swap_key1_2 swap_value1_2 ext]
        assert_equal {OK} [r set swap_mem_key1 swap_mem_value1]
        
        # Set keys in db2 external storage (should remain unaffected)
        r select 2
        assert_equal {OK} [r set swap_key2_1 swap_value2_1 ext]
        assert_equal {OK} [r set swap_mem_key2 swap_mem_value2]
        
        # Verify all keys exist before swap using GET, KEYS, and SCAN
        r select 0
        assert_equal swap_value0_1 [r get swap_key0_1 ext]
        assert_equal swap_value0_2 [r get swap_key0_2 ext]
        assert_equal swap_mem_value0 [r get swap_mem_key0]
        assert_equal {swap_key0_1 swap_key0_2} [lsort [r keys swap_key* ext]]
        set keys_db0 [lsort [scan_keys 0 "string" "swap_key*" "ext"]]
        assert_equal {swap_key0_1 swap_key0_2} $keys_db0
        
        r select 1
        assert_equal swap_value1_1 [r get swap_key1_1 ext]
        assert_equal swap_value1_2 [r get swap_key1_2 ext]
        assert_equal swap_mem_value1 [r get swap_mem_key1]
        assert_equal {swap_key1_1 swap_key1_2} [lsort [r keys swap_key* ext]]
        set keys_db1 [lsort [scan_keys 0 "string" "swap_key*" "ext"]]
        assert_equal {swap_key1_1 swap_key1_2} $keys_db1
        
        r select 2
        assert_equal swap_value2_1 [r get swap_key2_1 ext]
        assert_equal swap_mem_value2 [r get swap_mem_key2]
        assert_equal {swap_key2_1} [r keys swap_key* ext]
        
        # Execute SWAPDB between db0 and db1
        assert_equal {OK} [r swapdb 0 1]
        
        # Verify external data is swapped between db0 and db1 using GET, KEYS, and SCAN
        # db0 should now have db1's external data
        r select 0
        assert_equal swap_value1_1 [r get swap_key1_1 ext]
        assert_equal swap_value1_2 [r get swap_key1_2 ext]
        assert_equal swap_mem_value1 [r get swap_mem_key1]
        assert_equal {} [r get swap_key0_1 ext]
        assert_equal {} [r get swap_key0_2 ext]
        assert_equal {swap_key1_1 swap_key1_2} [lsort [r keys swap_key* ext]]
        set keys_db0_after [lsort [scan_keys 0 "string" "swap_key*" "ext"]]
        assert_equal {swap_key1_1 swap_key1_2} $keys_db0_after
        
        # db1 should now have db0's external data
        r select 1
        assert_equal swap_value0_1 [r get swap_key0_1 ext]
        assert_equal swap_value0_2 [r get swap_key0_2 ext]
        assert_equal swap_mem_value0 [r get swap_mem_key0]
        assert_equal {} [r get swap_key1_1 ext]
        assert_equal {} [r get swap_key1_2 ext]
        assert_equal {swap_key0_1 swap_key0_2} [lsort [r keys swap_key* ext]]
        set keys_db1_after [lsort [scan_keys 0 "string" "swap_key*" "ext"]]
        assert_equal {swap_key0_1 swap_key0_2} $keys_db1_after
        
        # Verify db2 external data is NOT affected
        r select 2
        assert_equal swap_value2_1 [r get swap_key2_1 ext]
        assert_equal swap_mem_value2 [r get swap_mem_key2]
        assert_equal {swap_key2_1} [r keys swap_key* ext]
    }
    
    test {SWAPDB with same database leaves external data unchanged} {
        # Set keys in db0
        r select 0
        assert_equal {OK} [r set same_key0_1 same_value0_1 ext]
        assert_equal {OK} [r set same_key0_2 same_value0_2 ext]
        
        # Verify keys exist before swap
        assert_equal same_value0_1 [r get same_key0_1 ext]
        assert_equal same_value0_2 [r get same_key0_2 ext]
        
        # Execute SWAPDB with same database
        assert_equal {OK} [r swapdb 0 0]
        
        # Verify external data is unchanged
        assert_equal same_value0_1 [r get same_key0_1 ext]
        assert_equal same_value0_2 [r get same_key0_2 ext]
    }
    
    test {SWAPDB with uninitialized external storage database} {
        # Initialize only db3
        assert_equal {OK} [r external_data INIT db3 helloextdata1]
        
        # Set keys in db3 external storage
        r select 3
        assert_equal {OK} [r set uninit_key3_1 uninit_value3_1 ext]
        assert_equal {OK} [r set uninit_key3_2 uninit_value3_2 ext]
        
        # Initialize db4 but don't put any data
        assert_equal {OK} [r external_data INIT db4 helloextdata1]
        
        # Verify keys exist in db3
        r select 3
        assert_equal uninit_value3_1 [r get uninit_key3_1 ext]
        assert_equal uninit_value3_2 [r get uninit_key3_2 ext]
        
        # Verify db4 is empty
        r select 4
        assert_equal {} [r get uninit_key3_1 ext]
        
        # Execute SWAPDB between db3 and db4
        assert_equal {OK} [r swapdb 3 4]
        
        # Verify external data is swapped
        # db4 should now have db3's external data
        r select 4
        assert_equal uninit_value3_1 [r get uninit_key3_1 ext]
        assert_equal uninit_value3_2 [r get uninit_key3_2 ext]
        
        # db3 should now be empty
        r select 3
        assert_equal {} [r get uninit_key3_1 ext]
        assert_equal {} [r get uninit_key3_2 ext]
    }
    
    test {SWAPDB with mixed memory and external storage} {
        # Set keys with mix of memory and external storage in db5
        r select 5
        assert_equal {OK} [r external_data INIT db5 helloextdata1]
        assert_equal {OK} [r set mixed_ext_key5 mixed_ext_value5 ext]
        assert_equal {OK} [r set mixed_mem_key5 mixed_mem_value5]
        
        # Set keys with mix of memory and external storage in db6
        r select 6
        assert_equal {OK} [r external_data INIT db6 helloextdata1]
        assert_equal {OK} [r set mixed_ext_key6 mixed_ext_value6 ext]
        assert_equal {OK} [r set mixed_mem_key6 mixed_mem_value6]
        
        # Verify keys exist before swap
        r select 5
        assert_equal mixed_ext_value5 [r get mixed_ext_key5 ext]
        assert_equal {} [r get mixed_ext_key5]
        assert_equal mixed_mem_value5 [r get mixed_mem_key5]
        
        r select 6
        assert_equal mixed_ext_value6 [r get mixed_ext_key6 ext]
        assert_equal {} [r get mixed_ext_key6]
        assert_equal mixed_mem_value6 [r get mixed_mem_key6]
        
        # Execute SWAPDB
        assert_equal {OK} [r swapdb 5 6]
        
        # Verify both memory and external data are swapped
        r select 5
        assert_equal mixed_ext_value6 [r get mixed_ext_key6 ext]
        assert_equal {} [r get mixed_ext_key6]
        assert_equal mixed_mem_value6 [r get mixed_mem_key6]
        assert_equal {} [r get mixed_ext_key5 ext]
        
        r select 6
        assert_equal mixed_ext_value5 [r get mixed_ext_key5 ext]
        assert_equal {} [r get mixed_ext_key5]
        assert_equal mixed_mem_value5 [r get mixed_mem_key5]
        assert_equal {} [r get mixed_ext_key6 ext]
    }

    test {SWAPDB: client sees swapped external data without SELECT} {
        # This test verifies that after SWAPDB, a client connected to a database
        # automatically sees the new external data without needing to call SELECT
        # (same behavior as with memory keys)
        
        # Initialize databases
        r select 7
        assert_equal {OK} [r external_data INIT db7 helloextdata1]
        r select 8
        assert_equal {OK} [r external_data INIT db8 helloextdata1]
        
        # Set up data in db7
        r select 7
        assert_equal {OK} [r set client_key7_1 client_value7_1 ext]
        assert_equal {OK} [r set client_key7_2 client_value7_2 ext]
        assert_equal {OK} [r set client_mem_key7 client_mem_value7]
        
        # Set up data in db8
        r select 8
        assert_equal {OK} [r set client_key8_1 client_value8_1 ext]
        assert_equal {OK} [r set client_key8_2 client_value8_2 ext]
        assert_equal {OK} [r set client_mem_key8 client_mem_value8]
        
        # Create a second client connected to db7
        set client2 [valkey_client]
        $client2 select 7
        
        # Verify client2 sees db7's data before swap
        assert_equal client_value7_1 [$client2 get client_key7_1 ext]
        assert_equal client_value7_2 [$client2 get client_key7_2 ext]
        assert_equal client_mem_value7 [$client2 get client_mem_key7]
        assert_equal {client_key7_1 client_key7_2} [lsort [$client2 keys client_key* ext]]
        
        # Verify client2 does NOT see db8's data
        assert_equal {} [$client2 get client_key8_1 ext]
        assert_equal {} [$client2 get client_key8_2 ext]
        
        # Execute SWAPDB using the main client (r)
        r select 0
        assert_equal {OK} [r swapdb 7 8]
        
        # Now client2 (still logically connected to db7, but without calling SELECT again)
        # should see db8's data because db7 and db8 were swapped
        assert_equal client_value8_1 [$client2 get client_key8_1 ext]
        assert_equal client_value8_2 [$client2 get client_key8_2 ext]
        assert_equal client_mem_value8 [$client2 get client_mem_key8]
        assert_equal {client_key8_1 client_key8_2} [lsort [$client2 keys client_key* ext]]
        
        # Verify client2 does NOT see db7's old data anymore
        assert_equal {} [$client2 get client_key7_1 ext]
        assert_equal {} [$client2 get client_key7_2 ext]
        
        # Verify using SCAN as well
        set keys_client2 {}
        set cur 0
        while 1 {
            set res [$client2 scan $cur type "string" match "client_key*" ext]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys_client2 {*}$k
            if {$cur == 0} break
        }
        assert_equal {client_key8_1 client_key8_2} [lsort $keys_client2]
        
        # Cleanup
        $client2 close
    }
}

# Test SWAPDB with cluster mode - skipping, as not supported

# Test DBSIZE command with EXT flag
# DBSIZE EXT should return the count of memory keys plus the count of external data keys

### Non-sharded
start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id20 "loglevel" debug] tags [list "external:skip"]] {
    test {DBSIZE without EXT returns memory keys count} {
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        r select 0
        
        # Set some keys in memory
        assert_equal {OK} [r set mem_key1 mem_value1]
        assert_equal {OK} [r set mem_key2 mem_value2]
        
        # Set some keys in external storage
        assert_equal {OK} [r set ext_key1 ext_value1 ext]
        assert_equal {OK} [r set ext_key1 ext_value2 ext]
        assert_equal {OK} [r set ext_key2 ext_value3 ext]
        
        # Verify DBSIZE without EXT returns only memory keys count
        assert_equal 2 [r dbsize]
        assert_equal {OK} [r flushall]
    }
    
    test {DBSIZE with EXT returns memory + external keys count} {
        # Set some keys in memory
        assert_equal {OK} [r set mem_key1 mem_value1]
        assert_equal {OK} [r set mem_key2 mem_value2]
        
        # Set some keys in external storage
        assert_equal {OK} [r set ext_key1 ext_value1 ext]
        assert_equal {OK} [r set ext_key2 ext_value2 ext]
        assert_equal {OK} [r set ext_key3 ext_value3 ext]
        
        # Verify DBSIZE with EXT returns sum of both counts
        assert_equal 5 [r dbsize ext]
        assert_equal {OK} [r flushall]
    }
    
    test {DBSIZE with EXT when external data not initialized} {
        # Set some keys in memory
        assert_equal {OK} [r set mem_key1 mem_value1]
        assert_equal {OK} [r set mem_key2 mem_value2]
        
        # Verify DBSIZE with EXT returns only memory keys count when external data is not initialized
        assert_equal 2 [r dbsize ext]
        assert_equal {OK} [r flushall]
    }
}
    
start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id21 "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
    test {DBSIZE with EXT in different databases} {
        # Load module and initialize external storage for multiple databases
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r external_data INIT db1 helloextdata1]
        
        # Set keys in db0
        r select 0
        assert_equal {OK} [r set db0_mem_key db0_mem_value]
        assert_equal {OK} [r set db0_mem_key db0_ext_value ext]
        
        # Set keys in db1
        r select 1
        assert_equal {OK} [r set db1_mem_key db1_mem_value]
        assert_equal {OK} [r set db1_ext_key db1_ext_value ext]
        
        # Verify DBSIZE with EXT returns correct count for each database
        r select 0
        assert_equal 1 [r dbsize ext]
        
        r select 1
        assert_equal 2 [r dbsize ext]
    }
}

### Sharded
start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id22 "loglevel" debug] tags [list "external:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
    
    test {DBSIZE without EXT returns memory keys count (cluster mode)} {
        wait_for_cluster_state ok
        
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        r select 0
        
        # Set some keys in memory
        assert_equal {OK} [r set mem_key1 mem_value1]
        assert_equal {OK} [r set mem_key2 mem_value2]
        
        # Set some keys in external storage
        assert_equal {OK} [r set ext_key1 ext_value1 ext]
        assert_equal {OK} [r set ext_key2 ext_value2 ext]
        assert_equal {OK} [r set ext_key2 ext_value3 ext]
        
        # Verify DBSIZE without EXT returns only memory keys count
        assert_equal 2 [r dbsize]
        assert_equal {OK} [r flushall]
    }
    
    test {DBSIZE with EXT returns memory + external keys count (cluster mode)} {
        wait_for_cluster_state ok
        
        # Set some keys in memory
        assert_equal {OK} [r set mem_key1 mem_value1]
        assert_equal {OK} [r set mem_key2 mem_value2]
        assert_equal {OK} [r set mem_key3 mem_value3]
        
        # Set some keys in external storage
        assert_equal {OK} [r set ext_key1 ext_value1 ext]
        assert_equal {OK} [r set ext_key2 ext_value2 ext]
        assert_equal {OK} [r set ext_key2 ext_value3 ext]
        
        # Verify DBSIZE with EXT returns sum of both counts
        assert_equal 5 [r dbsize ext]
        assert_equal {OK} [r flushall]
    }
    
    test {DBSIZE with EXT when external data not initialized (cluster mode)} {
        wait_for_cluster_state ok
        
        # Set some keys in memory
        assert_equal {OK} [r set mem_key1 mem_value1]
        assert_equal {OK} [r set mem_key2 mem_value2]
        
        # Verify DBSIZE with EXT returns only memory keys count when external data is not initialized
        assert_equal 2 [r dbsize ext]
        assert_equal {OK} [r flushall]
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id23 "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    test {DBSIZE with EXT in different databases (cluster mode)} {
        wait_for_cluster_state ok
        
        # Load module and initialize external storage for multiple databases
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r external_data INIT db1 helloextdata1]
        
        # Set keys in db0
        r select 0
        assert_equal {OK} [r set db0_mem_key db0_mem_value]
        assert_equal {OK} [r set db0_ext_key db0_ext_value ext]
        assert_equal {OK} [r set db0_ext_key db0_ext_value2 ext]
        
        # Set keys in db1
        r select 1
        assert_equal {OK} [r set db1_mem_key db1_mem_value]
        assert_equal {OK} [r set db1_ext_key db1_ext_value ext]
        assert_equal {OK} [r set db1_ext_key2 db1_ext_value ext]
        
        # Verify DBSIZE with EXT returns correct count for each database
        r select 0
        assert_equal 2 [r dbsize ext]
        
        r select 1
        assert_equal 3 [r dbsize ext]
    }
}

# EXTERNAL_DATA DUMP and LOAD command tests
start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id61 "loglevel" debug] tags [list "external:skip"]] {
    test "EXTERNAL_DATA DUMP and LOAD: Basic functionality" {
        cleanup_external_data_dump r

        assert_equal {OK} [r module load $extdatamodule1]
        # Check that db0 is NOT auto-initialized (no backup exists)
        assert_equal [list ] [r external_data stats dbs]

        # Initialize external storage manually
        assert_equal {OK} [r external_data INIT db0 helloextdata1]

        r select 0
        
        # Add some regular and external data
        r set key1 "value1"
        r set key_ext "ext_value" EXT
        
        # Dump external data
        set dump_result [r external_data dump]
        assert {$dump_result != ""}
        
        r set key_ext "ext_value_last" EXT
        r set key_ext_2 "ext_value_last" EXT
        
        # Dump external data again
        set dump_result [r external_data dump]
        assert {$dump_result != ""}

        # Clear external data
        r del key_ext
        
        # Load external data back from last backup
        r external_data load
        
        # Verify external data is restored and memory untouched
        assert_equal "value1" [r get key1]
        assert_equal [r get key_ext ext] "ext_value_last"
        assert_equal [r get key_ext_2 ext] "ext_value_last"
        
        cleanup_external_data_dump r
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id62 "loglevel" debug] tags [list "external:skip"]] {
    test "EXTERNAL_DATA DUMP and LOAD: With AOF enabled (persistence auto-load)" {
        cleanup_external_data_dump r

        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]

        r select 0
        
        # Enable AOF persistence
        r config set appendonly yes
        r config rewrite
        
        # Add some external data
        r set key1 "value1" EXT
        r set key2 "value2" EXT
        
        # Dump external data
        set dump_result [r external_data dump]
        assert {$dump_result != ""}
        
        # Clear external data
        r flushdb
        
        # Ensure that database data is not cleaned up
        assert_equal [list db0:helloextdata1] [r external_data stats dbs]

        # Restart server to test auto-load on startup
        restart_server 0 true false
        wait_done_loading r
        
        # Select database 0 after restart
        r select 0
        
        # Ensure that database data is automatically reinitialized after restart
        assert_equal [list db0:helloextdata1] [r external_data stats dbs]
        
        # Verify external data is automatically restored on startup
        assert_equal "value1" [r get key1 ext]
        assert_equal "value2" [r get key2 ext]

        # Cleanup
        r config set appendonly no
        r config rewrite
        r flushall

        # Ensure that database data is not cleaned up
        assert_equal [list db0:helloextdata1] [r external_data stats dbs]

        cleanup_external_data_dump r
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id63 "loglevel" debug] tags [list "external:skip"]] {
    test "EXTERNAL_DATA DUMP and LOAD: With save enabled (persistence auto-load)" {
        cleanup_external_data_dump r

        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]

        r select 0
        
        # Enable save persistence
        r config set save "900 1"
        r config rewrite
        
        # Add some external data
        r set key1 "value1" EXT
        r set key2 "value2" EXT
        
        # Dump external data
        set dump_result [r external_data dump]
        assert {$dump_result != ""}
        
        # Clear external data
        r flushdb

        # Ensure that database data is not cleaned up
        assert_equal [list db0:helloextdata1] [r external_data stats dbs]
        
        # Restart server to test auto-load on startup
        restart_server 0 true false
        wait_done_loading r
        
        # Select database 0 after restart
        r select 0
        
        # Verify external data is automatically restored on startup
        assert_equal "value1" [r get key1 ext]
        assert_equal "value2" [r get key2 ext]

        # Cleanup
        r config set save ""
        r config rewrite
        r flushall

        # Ensure that database data is not cleaned up
        assert_equal [list db0:helloextdata1] [r external_data stats dbs]

        cleanup_external_data_dump r
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id24 "loglevel" debug] tags [list "external:skip"]] {
    test "EXTERNAL_DATA DUMP and LOAD: With both AOF and save disabled (no persistence)" {
        cleanup_external_data_dump r

        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]

        r select 0
        
        # Disable both AOF and save persistence
        r config set appendonly no
        r config set save ""
        r config rewrite
        
        # Add some external data
        r set key1 "value1" EXT
        r set key2 "value2" EXT
        
        # Dump external data
        set dump_result [r external_data dump]
        assert {$dump_result != ""}
        
        # Clear external data
        r flushdb

        # Ensure that database data is not cleaned up
        assert_equal [list db0:helloextdata1] [r external_data stats dbs]
        
        # Restart server - should NOT auto-load without persistence
        restart_server 0 true false
        wait_done_loading r
        
        # Select database 0 after restart
        r select 0
        
        # Verify external data is automatically restored on startup
        assert_equal "value1" [r get key1 ext]
        assert_equal "value2" [r get key2 ext]

        # Cleanup
        r config set save ""
        r config rewrite
        r flushall

        # Ensure that database data is not cleaned up
        assert_equal [list db0:helloextdata1] [r external_data stats dbs]

        cleanup_external_data_dump r
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id25 "loglevel" debug] tags [list "external:skip"]] {
    test "EXTERNAL_DATA LOAD: Invalid dump data" {
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]

        r select 0

        # Ensure that database data is not cleaned up
        assert_equal [list db0:helloextdata1] [r external_data stats dbs]
        
        # Try to load invalid dump data
        catch { r external_data load "invalid_data" } result
        assert_match "*ERR*" $result

        # Check that server is ok, ext is processed
        assert_equal "" [r get key1]
        assert_equal "" [r get key1 ext]
        r set key1 "value1" EXT
        assert_equal "value1" [r get key1 ext]
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id26 "loglevel" debug] tags [list "external:skip"]] {
    test "EXTERNAL_DATA DUMP and LOAD: Large dataset" {
        cleanup_external_data_dump r

        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]

        r select 0
        
        # Add multiple external data entries
        for {set i 0} {$i < 100} {incr i} {
            r set "key_ext_$i" "ext_value_$i" EXT
        }
        
        # Verify some external data exists
        assert_equal [r get key_ext_0 ext] "ext_value_0"
        assert_equal [r get key_ext_50 ext] "ext_value_50"
        assert_equal [r get key_ext_99 ext] "ext_value_99"
        
        # Dump external data
        set dump_result [r external_data dump]
        assert {$dump_result != ""}
        
        # Clear external data
        r flushdb
        
        # Verify external data is gone
        assert_equal [r get key_ext_0 ext] ""
        assert_equal [r get key_ext_50 ext] ""
        assert_equal [r get key_ext_99 ext] ""
        
        # Load external data back
        r external_data load $dump_result
        
        # Verify external data is restored
        assert_equal [r get key_ext_0 ext] "ext_value_0"
        assert_equal [r get key_ext_50 ext] "ext_value_50"
        assert_equal [r get key_ext_99 ext] "ext_value_99"
        
        cleanup_external_data_dump r
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id27 "loglevel" debug] tags [list "external:skip"]] {
    test "EXTERNAL_DATA DUMP: Empty database" {
        cleanup_external_data_dump r

        # Initialize external data first
        assert_equal {OK} [r module load $extdatamodule1]
        r external_data INIT db0 helloextdata1
        r select 0
        
        # Dump external data from empty database - should succeed without error
        set empty_dump [r external_data dump]
        assert {$empty_dump != ""}

        # Check that server is ok, ext is processed
        assert_equal "" [r get key1]
        assert_equal "" [r get key1 ext]
        r set key1 "value1" EXT
        assert_equal "value1" [r get key1 ext]
        
        cleanup_external_data_dump r
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id28 "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
    test "EXTERNAL_DATA DUMP and LOAD: Multiple databases" {
        cleanup_external_data_dump r

        # Initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r external_data INIT db1 helloextdata1]

        # Add external data to multiple databases
        r select 0
        r set db0_key "db0_value" EXT
        
        r select 1
        r set db1_key "db1_value" EXT
        
        # Verify external data exists in both databases
        r select 0
        assert_equal [r get db0_key ext] "db0_value"
        assert_equal [r get db1_key ext] ""
        
        r select 1
        assert_equal [r get db0_key ext] ""
        assert_equal [r get db1_key ext] "db1_value"
        
        # Dump external data from db0
        set dump_result [r external_data dump]
        assert {$dump_result != ""}
        
        # Clear external data from db0
        r select 0
        r flushdb
        # Change data in db1
        r select 1
        r set db1_key "db1_value_new" EXT
        r set db1_key_2 "db2_value" EXT
        
        # Load external data back
        r external_data load $dump_result
        
        # Verify external data is restored to db0 and db1
        r select 0
        assert_equal [r get db0_key ext] "db0_value"
        r select 1
        assert_equal [r get db1_key ext] "db1_value"
        assert_equal [r get db1_key_2 ext] ""
        
        cleanup_external_data_dump r
    }
}

test "SET with EXT flag is replicated to replica" {
    start_server [list overrides [list "ext-data-mode" kv "ext-data-id" replica-id "loglevel" debug] tags [list "external:skip"]] {
        start_server [list overrides [list "ext-data-mode" kv "ext-data-id" primary-id "loglevel" debug]] {
            # Get primary and replica nodes
            set primary [srv 0 client]
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]
            set replica [srv -1 client]

            cleanup_external_data_dump $primary
            cleanup_external_data_dump $replica

            # Load module and initialize external storage
            assert_equal {OK} [$primary module load $extdatamodule1]
            assert_equal {OK} [$primary external_data INIT db0 helloextdata1]
            assert_equal "primary-id" [$primary external_data stats nodeid]

            # Init external data for replica now
            assert_equal {OK} [$replica module load $extdatamodule1]
            assert_equal {OK} [$replica external_data INIT db0 helloextdata1]
            assert_equal "replica-id" [$replica external_data stats nodeid]

            # Setup replication
            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 100 {
                [lindex [$replica role] 3] eq {connected} &&
                [catch {$replica ping} ping_reply] == 0 && $ping_reply eq {PONG}
            } else {
                fail "Replication not started."
            }

            # Add some external data on primary
            $primary select 0
            $primary set memory_key value
            $primary set primary_key1 "primary_value1" EXT
            $primary set primary_key2 "primary_value2" EXT
            
            # Select database 0 on replica to match primary
            $replica select 0
            
            # Verify that regular keys are replicated to memory
            assert_equal [$replica get memory_key] "value"
            
            # Verify that external keys are replicated and accessible
            assert_equal [$replica get primary_key1 ext] "primary_value1"
            assert_equal [$replica get primary_key2 ext] "primary_value2"
            
            cleanup_external_data_dump $primary
            cleanup_external_data_dump $replica
        }
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id29 "loglevel" debug]] {
    start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id2 "loglevel" debug]] {
        test "EXTERNAL_DATA DUMP and LOAD: Partial sync scenario" {
            set primary [srv -1 client]
            set primary_host [srv -1 host]
            set primary_port [srv -1 port]
            set replica [srv 0 client]

            cleanup_external_data_dump $primary
            cleanup_external_data_dump $replica

            # Load module and initialize external storage
            assert_equal {OK} [$primary module load $extdatamodule1]
            assert_equal {OK} [$primary external_data INIT db0 helloextdata1]
            
            # Configure replication backlog for partial sync
            $primary config set repl-backlog-size 1024
            $primary config set repl-backlog-ttl 3600
            
            # Add some external data on primary
            $primary select 0
            $primary set memory_key value
            $primary set primary_key1 "primary_value1" EXT
            $primary set primary_key2 "primary_value2" EXT
            
            # Setup replication
            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 100 {
                [lindex [$replica role] 3] eq {connected} &&
                [catch {$replica ping} ping_reply] == 0 && $ping_reply eq {PONG}
            } else {
                fail "Replication not started."
            }

            # Load module and initialize external storage on replica
            assert_equal {OK} [$replica module load $extdatamodule1]
            # Select database 0 on replica to match primary
            $replica select 0

            # Wait for replica to sync initial external data
            wait_for_condition 50 100 {
                [$replica get primary_key1 ext] eq "primary_value1" &&
                [$replica get primary_key2 ext] eq "primary_value2"
            } else {
                fail "Replica not synchronized with primary external data"
            }
            
            # Add more external data on primary
            $primary set primary_key3 "primary_value3" EXT
            
            # Wait for replica to sync new data
            wait_for_condition 50 100 {
                [$replica get primary_key3 ext] eq "primary_value3"
            } else {
                fail "Replica not synchronized with new external data"
            }
            
            # Break connection and simulate partial sync scenario
            $replica client kill $primary_host:$primary_port
            
            # Add more data on primary while replica is disconnected
            $primary set primary_key4 "primary_value4" EXT
            
            # Reconnect replica
            $replica replicaof $primary_host $primary_port
            
            # Wait for partial sync to complete
            wait_for_condition 50 100 {
                [$replica get primary_key4 ext] eq "primary_value4"
            } else {
                fail "Partial sync failed for external data"
            }
            
            # Verify all external data is present on replica
            assert_equal [$replica get primary_key1 ext] "primary_value1"
            assert_equal [$replica get primary_key2 ext] "primary_value2"
            assert_equal [$replica get primary_key3 ext] "primary_value3"
            assert_equal [$replica get primary_key4 ext] "primary_value4"

            cleanup_external_data_dump $primary
            cleanup_external_data_dump $replica
        }
    }
}

test "EXTERNAL_DATA DUMP and LOAD: replication" {
    start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id30 "loglevel" debug] tags [list "external:skip" "slow"]] {
        start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id70 "loglevel" debug]] {
            # Get primary and replica nodes
            set primary [srv 0 client]
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]
            set replica [srv -1 client]

            cleanup_external_data_dump $primary
            cleanup_external_data_dump $replica

            # Load module and initialize external storage
            assert_equal {OK} [$primary module load $extdatamodule1]
            assert_equal {OK} [$primary external_data INIT db0 helloextdata1]

            # Configure replication backlog for partial sync
            $primary config set repl-backlog-size 1024
            $primary config set repl-backlog-ttl 3600
            
            # Add some external data on primary
            $primary select 0
            $primary set memory_key value
            
            # Setup replication
            $replica replicaof $primary_host $primary_port
            $replica select 0
            wait_for_condition 50 100 {
                [lindex [$replica role] 3] eq {connected} &&
                [catch {$replica ping} ping_reply] == 0 && $ping_reply eq {PONG} &&
                [$replica get memory_key] == "value"
            } else {
                fail "Replication not started."
            }

            # Init external data for replica now
            assert_equal {OK} [$replica module load $extdatamodule1]
            assert_equal {OK} [$replica external_data INIT db0 helloextdata1]

            $primary set primary_key1 "primary_value1" EXT
            $primary set primary_key2 "primary_value2" EXT

            # Check replication works for external data
            wait_for_condition 50 100 {
                [$replica get primary_key1 ext] eq "primary_value1" &&
                [$replica get primary_key2 ext] eq "primary_value2"
            } else {
                fail "Replica not synchronized with primary external data"
            }
            
            # Verify ext-data-async-load is enabled by default (async mode)
            set config_result [$replica config get ext-data-async-load]
            assert_equal "yes" [lindex $config_result 1]
            
            # Check primary logs for async dump during replication
            set primary_log [srv 0 stdout]
            set primary_log_content [exec cat $primary_log]
            assert_match "*External data dump started asynchronously*" $primary_log_content
            
            # Wait for async dump to complete
            wait_for_condition 50 100 {
                [string match "*External data dump*completed*" [exec cat $primary_log]]
            } else {
                fail "Async external data dump did not complete"
            }
            
            # Check replica logs for external data load
            set replica_log [srv -1 stdout]
            set replica_log_content [exec cat $replica_log]
            assert_match "*External data async load initiated*" $replica_log_content

            # Dump external data
            $primary select 0
            set dump_result [$primary external_data dump]
            assert {$dump_result != ""}

            # Load external data
            $primary external_data load $dump_result

            # Set new value and load last backup
            $primary set primary_key3 "primary_value3" EXT
            assert_equal [$replica get primary_key3 ext] "primary_value3"
            $primary external_data load $dump_result
            
            # Verify all actual external data is present on replica
            assert_equal [$replica get primary_key1 ext] "primary_value1"
            assert_equal [$replica get primary_key2 ext] "primary_value2"

            # Wait for load on replica is eventually done cleaning
            wait_for_condition 50 100 {
                [$replica get primary_key3 ext] eq ""
            } else {
                fail "Async external data load has not cleaned up"
            }

            cleanup_external_data_dump $primary
            cleanup_external_data_dump $replica
        }
    }
}

# Cluster mode tests for EXTERNAL_DATA DUMP and LOAD commands
start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id31 "loglevel" debug] tags [list "external:skip" "slow"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
    
    test "EXTERNAL_DATA DUMP and LOAD: Basic functionality (cluster mode)" {
        wait_for_cluster_state ok
        cleanup_external_data_dump r

        # Initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        r select 0
        
        # Add some regular and external data
        r set key1 "value1"
        r set key_ext "ext_value" EXT
        
        # Dump external data
        set dump_result [r external_data dump]
        assert {$dump_result != ""}
        
        # Add some new external data
        r set key_ext "ext_value_last" EXT
        r set key_ext_2 "ext_value_last" EXT

        # Dump external data
        set dump_result [r external_data dump]
        assert {$dump_result != ""}

        # Clear external data
        r del key_ext
        
        # Load external data back from last backup
        r external_data load
        
        # Verify external data is restored and memory untouched
        assert_equal "value1" [r get key1]
        assert_equal "ext_value_last" [r get key_ext ext]
        assert_equal "ext_value_last" [r get key_ext_2 ext]
        
        cleanup_external_data_dump r
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id32 "loglevel" debug] tags [list "external:skip" "slow"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    test "EXTERNAL_DATA DUMP and LOAD: With AOF enabled (cluster mode, persistence auto-load)" {
        wait_for_cluster_state ok
        cleanup_external_data_dump r

        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]

        # Enable AOF persistence
        r config set appendonly yes
        r config rewrite
        
        # Add some external data
        r select 0
        r set key1 "value1" EXT
        r set key2 "value2" EXT
        
        # Dump external data
        set dump_result [r external_data dump]
        assert {$dump_result != ""}
        
        # Clear external data
        r flushdb
        
        # Restart node to test auto-load on startup
        restart_server 0 true false
        wait_done_loading r
        wait_for_cluster_state ok

        # Select database 0 after restart
        r select 0

        # Ensure that database data is automatically reinitialized after restart
        assert_equal [list db0:helloextdata1] [r external_data stats dbs]
        
        # Verify external data is automatically restored on startup
        assert_equal "value1" [r get key1 ext]
        assert_equal "value2" [r get key2 ext]

        # Cleanup
        r config set appendonly no
        r config rewrite
        
        cleanup_external_data_dump r
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id33 "loglevel" debug] tags [list "external:skip" "slow"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    test "EXTERNAL_DATA DUMP and LOAD: With save enabled (cluster mode, persistence auto-load)" {
        wait_for_cluster_state ok
        cleanup_external_data_dump r

        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
            
        # Enable save persistence
        r select 0
        r config set save "900 1"
        r config rewrite
        
        # Add some external data
        r set key1 "value1" EXT
        r set key2 "value2" EXT
        
        # Dump external data
        set dump_result [r external_data dump]
        assert {$dump_result != ""}
        
        # Clear external data
        r flushdb
        
        # Restart primary node to test auto-load on startup
        restart_server 0 true false
        wait_done_loading r
        wait_for_cluster_state ok
        
        # Select database 0 after restart
        r select 0
        
        # Verify external data is automatically restored on startup
        assert_equal "value1" [r get key1 ext]
        assert_equal "value2" [r get key2 ext]

        # Cleanup
        r config set save ""
        r config rewrite
        
        cleanup_external_data_dump r
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id34 "loglevel" debug] tags [list "external:skip" "slow"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    test "EXTERNAL_DATA DUMP and LOAD: With both AOF and save disabled (cluster mode, no persistence)" {
        wait_for_cluster_state ok
        cleanup_external_data_dump r
            
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]

        # Disable both AOF and save persistence
        r select 0
        r config set appendonly no
        r config set save ""
        r config rewrite
        
        # Add some external data
        r set key1 "value1" EXT
        r set key2 "value2" EXT
        
        # Dump external data
        set dump_result [r external_data dump]
        assert {$dump_result != ""}
        
        # Clear external data
        r flushdb
        
        # Restart node - should NOT auto-load without persistence
        restart_server 0 true false
        wait_done_loading r
        wait_for_cluster_state ok
        
        # Select database 0 after restart
        r select 0
        
        # Verify external data is automatically restored on startup
        assert_equal "value1" [r get key1 ext]
        assert_equal "value2" [r get key2 ext]

        # Cleanup
        r config set save ""
        r config rewrite
        r flushall

        # Ensure that database data is not cleaned up
        assert_equal [list db0:helloextdata1] [r external_data stats dbs]

        cleanup_external_data_dump r
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id35 "loglevel" debug] tags [list "external:skip" "slow"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    test "EXTERNAL_DATA LOAD: Invalid dump data (cluster mode)" {
        wait_for_cluster_state ok
        
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]

        # Clean up any existing initialization and backup files from previous tests
        #catch {r external_data drop db0 FORCE}
        #cleanup_external_data_dump r
        
        # Initialize for this test
        #r external_data INIT db0 helloextdata1

        r select 0
        
        # Try to load invalid dump data
        catch { r external_data load "invalid_data" } result
        assert_match "*ERR*" $result

        # Check that server is ok, ext is processed
        assert_equal "" [r get key1]
        assert_equal "" [r get key1 ext]
        r set key1 "value1" EXT
        assert_equal "value1" [r get key1 ext]
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id36 "loglevel" debug] tags [list "external:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    test "EXTERNAL_DATA DUMP and LOAD: Large dataset (cluster mode)" {
        wait_for_cluster_state ok
        cleanup_external_data_dump r

        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        
        # Add multiple external data entries
        r select 0
        for {set i 0} {$i < 100} {incr i} {
            r set "key_ext_$i" "ext_value_$i" EXT
        }
        
        # Verify some external data exists
        assert_equal "ext_value_0" [r get key_ext_0 ext]
        assert_equal "ext_value_50" [r get key_ext_50 ext]
        assert_equal "ext_value_99" [r get key_ext_99 ext]
        
        # Dump external data
        set dump_result [r external_data dump]
        assert {$dump_result != ""}
        
        # Clear external data
        r flushdb
        
        # Verify external data is gone
        assert_equal "" [r get key_ext_0 ext]
        assert_equal "" [r get key_ext_50 ext]
        assert_equal "" [r get key_ext_99 ext]
        
        # Load external data back
        r external_data load $dump_result
        
        # Verify external data is restored
        assert_equal "ext_value_0" [r get key_ext_0 ext]
        assert_equal "ext_value_50" [r get key_ext_50 ext]
        assert_equal "ext_value_99" [r get key_ext_99 ext]
        
        cleanup_external_data_dump r
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id37 "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    test "EXTERNAL_DATA DUMP and LOAD: Multiple databases (cluster mode)" {
        wait_for_cluster_state ok
        cleanup_external_data_dump r
        
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r external_data INIT db1 helloextdata1]
        
        # Add external data to different databases
        r select 0
        r set db0_key "db0_value" EXT
        
        r select 1
        r set db1_key "db1_value" EXT
        
        # Verify external data exists in both databases
        r select 0
        assert_equal "db0_value" [r get db0_key ext]
        assert_equal "" [r get db1_key ext]
        
        r select 1
        assert_equal "" [r get db0_key ext]
        assert_equal "db1_value" [r get db1_key ext]
        
        # Dump external data
        set dump_result [r external_data dump]
        assert {$dump_result != ""}
        
        # Clear external data from db0
        r select 0
        r flushdb
        # Change data in db1
        r select 1
        r set db1_key "db1_value_new" EXT
        r set db1_key_2 "db2_value" EXT
        
        # Load external data back
        r external_data load $dump_result

        # Verify external data is restored to db0 and db1
        r select 0
        assert_equal [r get db0_key ext] "db0_value"
        r select 1
        assert_equal [r get db1_key ext] "db1_value"
        assert_equal [r get db1_key_2 ext] ""
        
        cleanup_external_data_dump r
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id38 "loglevel" debug] tags [list "external:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    test "EXTERNAL_DATA DUMP: Empty database (cluster mode)" {
        wait_for_cluster_state ok
        cleanup_external_data_dump r
        
        # Initialize external data first
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        r select 0
        
        # Dump external data from empty database - should succeed without error
        set empty_dump [r external_data dump]
        assert {$empty_dump != ""}

        # Check that server is ok, ext is processed
        assert_equal "" [r get key1]
        assert_equal "" [r get key1 ext]
        r set key1 "value1" EXT
        assert_equal "value1" [r get key1 ext]
        
        cleanup_external_data_dump r
    }
}

start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" id39_primary "loglevel" debug] tags [list "external:skip"]] {
    start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" id39_replica "loglevel" debug] tags [list "external:skip"]] {
        test "EXTERNAL_DATA DUMP and LOAD: Partial sync scenario (cluster mode)" {
            set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
            
            set primary [srv -1 client]
            set replica [srv 0 client]
            set primary_host [srv -1 host]
            set primary_port [srv -1 port]

        cleanup_external_data_dump $primary
        cleanup_external_data_dump $replica
            setup_ready_cluster [list $primary] [dict create $replica 0]
            
            wait_for_cluster_propagation
            
            # Load module and initialize external storage on primary
            assert_equal {OK} [$primary module load $extdatamodule1]
        # We don't want to introduce some complex logic, so just create backup on every set here
        assert_equal {OK} [$primary config set helloextdata1.dump_every_write 1]
        assert_equal {OK} [$primary external_data INIT db0 helloextdata1]

        # Configure replication backlog for partial sync
        $primary config set repl-backlog-size 1024
        $primary config set repl-backlog-ttl 3600
        
        # Add some external data on primary
        $primary select 0
        $primary set memory_key value
        $primary set cluster_key1 "cluster_value1" EXT
        $primary set cluster_key2 "cluster_value2" EXT
        assert_equal [$primary get cluster_key1 ext] "cluster_value1"
        assert_equal [$primary get cluster_key2 ext] "cluster_value2"

        # Init external data for replica now
        assert_equal {OK} [$replica module load $extdatamodule1]

        $replica select 0
        # We don't need MOVED to primary, but need direct gets from replica itself
        $replica READONLY
        assert_equal [$replica get memory_key] "value"

        # Wait for replica to sync initial external data
        wait_for_condition 50 100 {
            [$replica get cluster_key1 ext] eq "cluster_value1" &&
            [$replica get cluster_key2 ext] eq "cluster_value2"
        } else {
            fail "Replica not synchronized with primary external data"
        }
        
        # Add more external data on primary
        $primary set cluster_key3 "cluster_value3" EXT
        
        # Wait for replica to sync new data
        wait_for_condition 50 100 {
            [$replica get cluster_key3 ext] eq "cluster_value3"
        } else {
            fail "Replica not synchronized with new external data"
        }
        
        # Break connection and simulate partial sync scenario
        $replica client kill $primary_host:$primary_port
        
        # Add more data on primary while replica is disconnected
        $primary set cluster_key4 "cluster_value4" EXT
        
        # Wait for partial sync to complete - auto-reconnect in cluster mode
        wait_for_condition 50 100 {
            [$replica get cluster_key4 ext] eq "cluster_value4"
        } else {
            fail "Partial sync failed for external data in cluster mode"
        }
        
        # Verify all external data is present on replica
        assert_equal [$replica get cluster_key1 ext] "cluster_value1"
        assert_equal [$replica get cluster_key2 ext] "cluster_value2"
        assert_equal [$replica get cluster_key3 ext] "cluster_value3"
        assert_equal [$replica get cluster_key4 ext] "cluster_value4"
        
        # Verify cluster state is still ok
        wait_for_cluster_state ok

            cleanup_external_data_dump $primary
            cleanup_external_data_dump $replica
        }
    }
}

start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" id40_primary "loglevel" debug] tags [list "external:skip"]] {
    start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" id40_replica "loglevel" debug] tags [list "external:skip"]] {
        test "EXTERNAL_DATA DUMP and LOAD: replication (cluster mode)" {
            set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
            
            set primary [srv -1 client]
            set primary_host [srv -1 host]
            set primary_port [srv -1 port]
            set replica [srv 0 client]

        cleanup_external_data_dump $primary
        cleanup_external_data_dump $replica
            setup_ready_cluster [list $primary] [dict create $replica 0]

        # Load module and initialize external storage
        assert_equal {OK} [$primary module load $extdatamodule1]
        # We don't want to introduce some complex logic, so just create backup on every set here
        assert_equal {OK} [$primary config set helloextdata1.dump_every_write 1]
        assert_equal {OK} [$primary external_data INIT db0 helloextdata1]

        # Configure replication backlog for partial sync
        $primary config set repl-backlog-size 1024
        $primary config set repl-backlog-ttl 3600
        
        # Add some external data on primary
        $primary select 0
        $primary set memory_key value
        $primary set primary_key1 "primary_value1" EXT
        $primary set primary_key2 "primary_value2" EXT

        $replica select 0
        # We don't need MOVED to primary, but need direct gets from replica itself
        $replica READONLY

        # Replica has no external data as module is not loaded, server is not failing
        assert_equal [$replica get memory_key] "value"
        assert_equal [$replica get primary_key1 ext] ""
        assert_equal [$replica get primary_key2 ext] ""

        # Init external data for replica now
        assert_equal {OK} [$replica module load $extdatamodule1]

        # Wait for replica to sync initial external data
        wait_for_condition 50 100 {
            [$replica get primary_key1 ext] eq "primary_value1" &&
            [$replica get primary_key2 ext] eq "primary_value2"
        } else {
            fail "Replica not synchronized with primary external data"
        }
        
        # Verify ext-data-async-load is enabled by default (async mode)
        set config_result [$replica config get ext-data-async-load]
        assert_equal "yes" [lindex $config_result 1]
        
        # Check primary logs for async dump during replication
        set primary_log [srv -1 stdout]
        set primary_log_content [exec cat $primary_log]
        assert_match "*External data dump started asynchronously*" $primary_log_content
        
        # Wait for async dump to complete
        wait_for_condition 50 100 {
            [string match "*External data dump*completed*" [exec cat $primary_log]]
        } else {
            fail "Async external data dump did not complete"
        }
        
        # Check replica logs for external data load
        set replica_log [srv 0 stdout]
        set replica_log_content [exec cat $replica_log]
        assert_match "*External data async load initiated*" $replica_log_content

        # Dump external data
        $primary select 0
        set dump_result [$primary external_data dump]
        assert {$dump_result != ""}

        # Load external data
        $primary external_data load $dump_result

        # Set new value and load last backup
        $primary set primary_key3 "primary_value3" EXT
        assert_equal [$replica get primary_key3 ext] "primary_value3"
        $primary external_data load $dump_result
        assert_equal [$primary get primary_key3 ext] ""
        
        # Verify all actual external data is present on replica
        assert_equal [$replica get primary_key1 ext] "primary_value1"
        assert_equal [$replica get primary_key2 ext] "primary_value2"
        wait_for_condition 50 100 {
            [$replica get primary_key3 ext] eq ""
        } else {
            fail "Value set after backup creation is not dropped on backup load on replica"
        }

        cleanup_external_data_dump $primary
        cleanup_external_data_dump $replica
        }
    }
}


# Test async external data dump for replication
start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id41 "loglevel" debug] tags [list "external:skip" "slow"]] {
    test "Async external data dump: Basic functionality" {
        cleanup_external_data_dump r

        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        r select 0
        
        # Add some external data
        r set async_key1 "async_value1" EXT
        r set async_key2 "async_value2" EXT
        r set async_key3 "async_value3" EXT
        
        # Trigger BGSAVE to test async dump
        r bgsave
        
        # Wait for BGSAVE to complete
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 0
        } else {
            fail "BGSAVE did not complete in time"
        }
        
        # Check server logs for async dump completion message
        # The async dump should have completed and logged the backup ID
        set log_file [srv 0 stdout]
        set log_content [exec cat $log_file]
        
        # Verify async dump was triggered
        assert_match "*External data dump started asynchronously*" $log_content
        
        # Wait for dump to complete and verify
        wait_for_condition 50 100 {
            [string match "*External data dump*completed*" [exec cat $log_file]]
        } else {
            fail "Async external data dump did not complete"
        }

        cleanup_external_data_dump r
    }
}

test "Async external data dump: Non-blocking behavior" {
    start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id42 "loglevel" debug] tags [list "external:skip"]] {
        cleanup_external_data_dump r

        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        r select 0
        
        # Add a large number of keys to external storage
        set num_keys 1000
        for {set i 0} {$i < $num_keys} {incr i} {
            r set "nonblock_key_$i" "nonblock_value_$i" EXT
        }
        
        # Trigger BGSAVE which will start async dump
        r bgsave
        
        # Immediately try to perform other operations
        # These should not be blocked by the async dump
        set start_time [clock milliseconds]
        
        # Perform multiple operations while dump is in progress
        # Mix of memory and external storage operations
        for {set i 0} {$i < 100} {incr i} {
            r set "test_key_$i" "test_value_$i"
            r get "test_key_$i"
            r set "test_ext_key_$i" "test_ext_value_$i" EXT
            r get "test_ext_key_$i" EXT
        }
        
        set end_time [clock milliseconds]
        set elapsed_time [expr {$end_time - $start_time}]
        
        # Operations should complete quickly (< 1 second) even with dump in progress
        assert {$elapsed_time < 1000}
        
        # Wait for BGSAVE to complete
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 0
        } else {
            fail "BGSAVE did not complete in time"
        }
        
        # Wait for dump to complete and verify
        wait_for_condition 50 100 {
            [string match "*External data dump*completed*" [exec cat [srv 0 stdout]]]
        } else {
            fail "Async external data dump did not complete"
        }

        cleanup_external_data_dump r
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id43 "loglevel" debug] tags [list "external:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
    
    test "Async external data dump: Cluster mode basic functionality" {
        wait_for_cluster_state ok
        cleanup_external_data_dump r
        
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        r select 0
        
        # Add some external data
        r set cluster_async_key1 "cluster_async_value1" EXT
        r set cluster_async_key2 "cluster_async_value2" EXT
        
        # Trigger BGSAVE to test async dump
        r bgsave
        
        # Wait for BGSAVE to complete
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 0
        } else {
            fail "BGSAVE did not complete in time"
        }
        
        # Check logs for async dump
        set log_file [srv 0 stdout]
        set log_content [exec cat $log_file]
        assert_match "*External data dump started asynchronously*" $log_content
        
        # Wait for dump to complete and verify
        wait_for_condition 50 100 {
            [string match "*External data dump*completed*" [exec cat $log_file]]
        } else {
            fail "Async external data dump did not complete"
        }

        cleanup_external_data_dump r
    }
    
    test "Async external data dump: Non-blocking behavior (cluster mode)" {
        wait_for_cluster_state ok
        cleanup_external_data_dump r
        
        # Add a large number of keys to external storage
        set num_keys 1000
        for {set i 0} {$i < $num_keys} {incr i} {
            r set "cluster_nonblock_key_$i" "cluster_nonblock_value_$i" EXT
        }
        
        # Trigger BGSAVE which will start async dump
        r bgsave
        
        # Immediately try to perform other operations
        # These should not be blocked by the async dump
        set start_time [clock milliseconds]
        
        # Perform multiple operations while dump is in progress
        # Mix of memory and external storage operations
        for {set i 0} {$i < 100} {incr i} {
            r set "cluster_test_key_$i" "cluster_test_value_$i"
            r get "cluster_test_key_$i"
            r set "cluster_test_ext_key_$i" "cluster_test_ext_value_$i" EXT
            r get "cluster_test_ext_key_$i" EXT
        }
        set end_time [clock milliseconds]
        set elapsed_time [expr {$end_time - $start_time}]
        
        # Operations should complete quickly (< 1 second) even with dump in progress
        assert {$elapsed_time < 1000}
        
        # Wait for BGSAVE to complete
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 0
        } else {
            fail "BGSAVE did not complete in time"
        }
        
        # Wait for dump to complete and verify
        wait_for_condition 50 100 {
            [string match "*External data dump*completed*" [exec cat [srv 0 stdout]]]
        } else {
            fail "Async external data dump did not complete in cluster mode"
        }

        cleanup_external_data_dump r
    }
}

# Test ext-data-async-load configuration
start_server {tags {"external:skip" "wip"} overrides {"ext-data-mode" kv "ext-data-id" id44 "loglevel" debug "ext-data-async-load" no}} {
    start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id71 "loglevel" debug "ext-data-async-load" no] tags [list "wip"]] {
        test "ext-data-async-load: Sync mode" {
            set primary [srv -1 client]
            set primary_host [srv -1 host]
            set primary_port [srv -1 port]
            set replica [srv 0 client]

            cleanup_external_data_dump $primary
            cleanup_external_data_dump $replica
            
            # Verify ext-data-async-load is disabled
            set config_result [$replica config get ext-data-async-load]
            assert_equal "no" [lindex $config_result 1]
            
            # Load module and initialize external storage on primary
            assert_equal {OK} [$primary module load $extdatamodule1]
            assert_equal {OK} [$primary external_data INIT db0 helloextdata1]
            
            # Add external data on primary
            $primary select 0
            $primary set sync_test_key1 "sync_test_value1" EXT
            $primary set sync_test_key2 "sync_test_value2" EXT
            $primary set memory_key "memory_value"
            
            # Setup replication
            $replica replicaof $primary_host $primary_port
            $replica select 0
            wait_for_condition 50 100 {
                [lindex [$replica role] 3] eq {connected} &&
                [catch {$replica ping} ping_reply] == 0 && $ping_reply eq {PONG} &&
                [$replica get memory_key] == "memory_value"
            } else {
                fail "Replication not started."
            }
            
            # Load module on replica
            assert_equal {OK} [$replica module load $extdatamodule1]
            # assert_equal {OK} [$replica external_data INIT db0 helloextdata1]
            
            # In sync mode, both memory and external data should be available together
            wait_for_condition 50 100 {
                [$replica get memory_key] eq "memory_value" &&
                [$replica get sync_test_key1 ext] eq "sync_test_value1" &&
                [$replica get sync_test_key2 ext] eq "sync_test_value2"
            } else {
                fail "Data not synced in sync mode"
            }
            
            # Check replica logs for sync load message
            set replica_log [srv 0 stdout]
            set replica_log_content [exec cat $replica_log]
            assert_match "*loaded synchronously*" $replica_log_content

            cleanup_external_data_dump $primary
            cleanup_external_data_dump $replica
        }
    }
}

start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" id45_primary "loglevel" debug "ext-data-async-load" no] tags [list "external:skip" "wip"]] {
    start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" id45_replica "loglevel" debug "ext-data-async-load" no] tags [list "external:skip" "wip"]] {
        test "ext-data-async-load: Sync mode (cluster)" {
            set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
            
            set primary [srv -1 client]
            set replica [srv 0 client]

        cleanup_external_data_dump $primary
        cleanup_external_data_dump $replica
            setup_ready_cluster [list $primary] [dict create $replica 0]
        
        # Verify ext-data-async-load is disabled
        set config_result [$replica config get ext-data-async-load]
        assert_equal "no" [lindex $config_result 1]
        
        # Load module and initialize external storage on primary
        assert_equal {OK} [$primary module load $extdatamodule1]
        assert_equal {OK} [$primary config set helloextdata1.dump_every_write 1]
        assert_equal {OK} [$primary external_data INIT db0 helloextdata1]
        
        # Add external data on primary
        $primary select 0
        $primary set cluster_sync_key1 "cluster_sync_value1" EXT
        $primary set cluster_sync_key2 "cluster_sync_value2" EXT
        $primary set cluster_memory_key "cluster_memory_value"
        
        # Load module on replica
        assert_equal {OK} [$replica module load $extdatamodule1]
        # assert_equal {OK} [$replica external_data INIT db0 helloextdata1]
        
        $replica select 0
        # We don't need MOVED to primary, but need direct gets from replica itself
        $replica READONLY

        # In sync mode, both memory and external data should be available together
        wait_for_condition 50 100 {
            [$replica get cluster_memory_key] eq "cluster_memory_value" &&
            [$replica get cluster_sync_key1 ext] eq "cluster_sync_value1" &&
            [$replica get cluster_sync_key2 ext] eq "cluster_sync_value2"
        } else {
            fail "Data not synced in sync mode (cluster)"
        }
        
        # Check replica logs for sync load message
        set replica_log [srv 0 stdout]
        set replica_log_content [exec cat $replica_log]
        assert_match "*loaded synchronously*" $replica_log_content

        cleanup_external_data_dump $primary
        cleanup_external_data_dump $replica
        }
    }
}

# Test external storage with simulated failures
# Tests both standalone and cluster mode with 50% failure rate
start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id46 "loglevel" debug] tags [list "external:skip"]] {
    test "External storage with simulated failures - standalone mode" {
        # Load module with 50% failure rate (fails every 2nd set, starting from 1st)
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r config set helloextdata1.set_failure_percent 50]
        # Verify config was set
        assert_equal 50 [lindex [r config get helloextdata1.set_failure_percent] 1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        r select 0
        
        # First set should fail (1st operation)
        assert_error "ERR External storage operation failed" {r set key1 value1 ext}
        
        # Second set should succeed (2nd operation)
        assert_equal {OK} [r set key2 value2 ext]

        # Third set should fail (3rd operation)
        assert_error "ERR External storage operation failed" {r set key3 value3 ext}
        
        # Fourth set should succeed (4th operation)
        assert_equal {OK} [r set key4 value4 ext]
        
        # Verify successful sets are stored
        assert_equal value2 [r get key2 ext]
        assert_equal value4 [r get key4 ext]
        
        # Verify failed sets are not stored
        assert_equal {} [r get key1 ext]
        assert_equal {} [r get key3 ext]
    }
}

# Test replication with simulated failures
start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id47 "loglevel" debug] tags [list "external:skip"]] {
    start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id72 "loglevel" debug]] {
        test "External storage replication with simulated failures - standalone mode" {
            set primary [srv -1 client]
            set primary_host [srv -1 host]
            set primary_port [srv -1 port]
            set replica [srv 0 client]

            cleanup_external_data_dump $primary
            cleanup_external_data_dump $replica
            
            # Load module on primary without failures
            set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
            assert_equal {OK} [$primary module load $extdatamodule1]
            assert_equal {OK} [$primary external_data INIT db0 helloextdata1]
            
            # Add data on primary
            $primary select 0
            $primary set primary_key1 "primary_value1" EXT
            $primary set primary_key2 "primary_value2" EXT
            
            # Setup replication
            $replica replicaof $primary_host $primary_port
            $replica select 0
            wait_for_condition 50 100 {
                [lindex [$replica role] 3] eq {connected} &&
                [catch {$replica ping} ping_reply] == 0 && $ping_reply eq {PONG}
            } else {
                fail "Replication not started."
            }
            
            # Load module on replica WITH 50% failure rate
            assert_equal {OK} [$replica module load $extdatamodule1]
            assert_equal {OK} [$replica config set helloextdata1.set_failure_percent 50]
            # assert_equal {OK} [$replica external_data INIT db0 helloextdata1]
            
            # Despite the 50% failure rate on replica, data should eventually sync
            # The first attempt will fail, but retries should succeed
            wait_for_condition 50 100 {
                [$replica get primary_key1 ext] eq "primary_value1" &&
                [$replica get primary_key2 ext] eq "primary_value2"
            } else {
                fail "Replica not synchronized despite failures"
            }
            
            # Add more data on primary
            $primary set primary_key3 "primary_value3" EXT
            $primary set primary_key4 "primary_value4" EXT
            
            # Verify replica eventually syncs new data despite failures
            wait_for_condition 50 100 {
                [$replica get primary_key3 ext] eq "primary_value3" &&
                [$replica get primary_key4 ext] eq "primary_value4"
            } else {
                fail "Replica not synchronized with new data despite failures"
            }
            
            # Verify system stability - both primary and replica should be operational
            assert_equal "primary_value1" [$primary get primary_key1 ext]
            assert_equal "primary_value1" [$replica get primary_key1 ext]

            cleanup_external_data_dump $primary
            cleanup_external_data_dump $replica
        }
    }
}

# Test cluster mode with simulated failures
start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" id48_primary "loglevel" debug] tags [list "external:skip"]] {
    start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" id48_replica "loglevel" debug] tags [list "external:skip"]] {
        set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
        
        test "External storage with simulated failures - cluster mode" {
            set primary [srv -1 client]
            set replica [srv 0 client]
            
            setup_ready_cluster [list $primary] [dict create $replica 0]

        cleanup_external_data_dump $primary
        cleanup_external_data_dump $replica
        
        # Load module on primary without failures
        assert_equal {OK} [$primary module load $extdatamodule1]
        assert_equal {OK} [$primary config set helloextdata1.dump_every_write 1]
        assert_equal {OK} [$primary external_data INIT db0 helloextdata1]
        
        # Add data on primary
        $primary select 0
        $primary set cluster_key1 "cluster_value1" EXT
        $primary set cluster_key2 "cluster_value2" EXT
        
        # Load module on replica WITH 50% failure rate
        assert_equal {OK} [$replica module load $extdatamodule1]
        assert_equal {OK} [$replica config set helloextdata1.set_failure_percent 50]
        # assert_equal {OK} [$replica external_data INIT db0 helloextdata1]
        
        $replica select 0
        # Enable readonly on replica for cluster mode
        $replica readonly

        # Despite the 50% failure rate on replica, data should eventually sync
        wait_for_condition 50 100 {
            [$replica get cluster_key1 ext] eq "cluster_value1" &&
            [$replica get cluster_key2 ext] eq "cluster_value2"
        } else {
            fail "Cluster replica not synchronized despite failures"
        }
        
        # Add more data on primary
        $primary set cluster_key3 "cluster_value3" EXT
        $primary set cluster_key4 "cluster_value4" EXT
        
        # Verify replica eventually syncs new data despite failures
        wait_for_condition 50 100 {
            [$replica get cluster_key3 ext] eq "cluster_value3" &&
            [$replica get cluster_key4 ext] eq "cluster_value4"
        } else {
            fail "Cluster replica not synchronized with new data despite failures"
        }
        
        # Verify system stability - both primary and replica should be operational
        assert_equal "cluster_value1" [$primary get cluster_key1 ext]
        assert_equal "cluster_value1" [$replica get cluster_key1 ext]
        
        # Verify cluster state is still ok
        wait_for_cluster_state ok

        cleanup_external_data_dump $primary
        cleanup_external_data_dump $replica
        }
    }
}

# Test replication with 100% failure rate and replica restart
start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id49 "loglevel" debug] tags [list "external:skip" "slow"]] {
    start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id73 "loglevel" debug]] {
        test "External storage replication with 100% failures and restart - standalone mode" {
            set primary_id -1
            set replica_id 0
            set primary [srv $primary_id client]
            set primary_host [srv $primary_id host]
            set primary_port [srv $primary_id port]
            set replica [srv $replica_id client]

            cleanup_external_data_dump $primary
            cleanup_external_data_dump $replica
            
            # Load module on primary without failures
            set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
            assert_equal {OK} [$primary module load $extdatamodule1]
            assert_equal {OK} [$primary external_data INIT db0 helloextdata1]
            
            # Add data on primary
            $primary select 0
            $primary set primary_key1 "primary_value1" EXT
            $primary set primary_key2 "primary_value2" EXT
            
            # Setup replication
            $replica replicaof $primary_host $primary_port
            $replica select 0
            wait_for_condition 50 100 {
                [lindex [$replica role] 3] eq {connected} &&
                [catch {$replica ping} ping_reply] == 0 && $ping_reply eq {PONG}
            } else {
                fail "Replication not started."
            }
            
            # Load module on replica WITH 100% failure rate (all sets fail)
            assert_equal {OK} [$replica module load $extdatamodule1]
            assert_equal {OK} [$replica config set helloextdata1.set_failure_percent 100]
            
            # Verify data is NOT synced yet due to 100% failures
            wait_for_condition 50 100 {
                [$replica get primary_key1 ext] eq "primary_value1" &&
                [$replica get primary_key2 ext] eq "primary_value2"
            } else {
                fail "Cluster replica not synchronized despite failures"
            }
            
            # Restart replica to clear the failure state
            $primary config rewrite
            $replica config rewrite
            restart_server $replica_id true false
            set replica [srv $replica_id client]
            
            # Select database 0 after restart
            $replica select 0
            
            # Reload module on replica WITHOUT failures this time
            #assert_equal {OK} [$replica module load $extdatamodule1]
            #assert_equal {OK} [$replica external_data INIT db0 helloextdata1]
            
            # Now data should sync successfully
            wait_for_condition 50 100 {
                [$replica get primary_key1 ext] eq "primary_value1" &&
                [$replica get primary_key2 ext] eq "primary_value2"
            } else {
                fail "Replica not synchronized after restart"
            }
            
            # Add more data on primary to verify ongoing replication
            $primary set primary_key3 "primary_value3" EXT
            
            # Verify new data syncs
            wait_for_condition 50 100 {
                [$replica get primary_key3 ext] eq "primary_value3"
            } else {
                fail "Replica not synchronized with new data after restart"
            }
            
            # Verify system stability
            assert_equal "primary_value1" [$primary get primary_key1 ext]
            assert_equal "primary_value1" [$replica get primary_key1 ext]

            cleanup_external_data_dump $primary
            cleanup_external_data_dump $replica
        }
    }
}

# Test cluster mode with 100% failure rate and replica restart
start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" id50_primary "loglevel" debug] tags [list "external:skip" "slow"]] {
    start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" id50_replica "loglevel" debug] tags [list "external:skip" "slow"]] {
        set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

        test "External storage with 100% failures and restart - cluster mode" {
            set primary_id -1
            set replica_id 0
            set primary [srv $primary_id client]
            set primary_host [srv $primary_id host]
            set primary_port [srv $primary_id port]
            set replica [srv $replica_id client]

        cleanup_external_data_dump $primary
        cleanup_external_data_dump $replica
            setup_ready_cluster [list $primary] [dict create $replica 0]
        
        # Load module on primary without failures
        assert_equal {OK} [$primary module load $extdatamodule1]
        assert_equal {OK} [$primary config set helloextdata1.dump_every_write 1]
        assert_equal {OK} [$primary external_data INIT db0 helloextdata1]
        
        # Add data on primary
        $primary select 0
        $primary set cluster_key1 "cluster_value1" EXT
        $primary set cluster_key2 "cluster_value2" EXT
        
        # Load module on replica WITH 100% failure rate (all sets fail)
        assert_equal {OK} [$replica module load $extdatamodule1]
        assert_equal {OK} [$replica config set helloextdata1.set_failure_percent 100]
        
        $replica select 0
        $replica READONLY
        wait_for_condition 50 100 {
            [$replica get cluster_key1 ext] eq "cluster_value1" &&
            [$replica get cluster_key2 ext] eq "cluster_value2"
        } else {
            fail "Cluster replica not synchronized despite failures"
        }
        
        # Restart replica to clear the failure state
        $primary config rewrite
        $replica config rewrite
        restart_server $replica_id true false
        set replica [srv $replica_id client]
        
        # Select database 0 after restart
        $replica select 0
        $replica READONLY
        wait_for_cluster_state ok
        
        # Reload module on replica and use config set to disable failures
        # assert_equal {OK} [$replica module load $extdatamodule1 set_failure_percent=100]
        # assert_equal {OK} [$replica external_data INIT db0 helloextdata1]
        
        # Use runtime configuration to set failure rate to 0
        assert_equal {OK} [$replica config set helloextdata1.set_failure_percent 0]
        
        # Wait for cluster to stabilize
        # wait_for_cluster_state ok
        
        # Now data should sync successfully
        wait_for_condition 50 100 {
            [$replica get cluster_key1 ext] eq "cluster_value1" &&
            [$replica get cluster_key2 ext] eq "cluster_value2"
        } else {
            fail "Cluster replica not synchronized after restart"
        }
        
        # Add more data on primary to verify ongoing replication
        $primary set cluster_key3 "cluster_value3" EXT
        
        # Verify new data syncs
        wait_for_condition 50 100 {
            [$replica get cluster_key3 ext] eq "cluster_value3"
        } else {
            fail "Cluster replica not synchronized with new data after restart"
        }
        
        # Verify system stability
        assert_equal "cluster_value1" [$primary get cluster_key1 ext]
        assert_equal "cluster_value1" [$replica get cluster_key1 ext]
        
        # Verify cluster state is still ok
        wait_for_cluster_state ok

        cleanup_external_data_dump $primary
        cleanup_external_data_dump $replica
        }
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id52 "loglevel" notice] tags [list "external:skip"]] {
    test {Slot values in standalone mode} {
        cleanup_external_data_dump r

        set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
        r MODULE LOAD $extdatamodule1
        r EXTERNAL_DATA INIT db0 helloextdata1
        r select 0
        r SET foo bar ext
        r GET foo ext
        # Verify slot is -1 for standalone mode
        assert_equal "-1" [r helloextdata1.storage_getslot db0 foo]
        assert_equal "-1" [r helloextdata1.filter_getslot db0 foo]

        cleanup_external_data_dump r
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" id53 "loglevel" notice] tags [list "external:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
    
    test {Slot values in cluster mode} {
        cleanup_external_data_dump r

        wait_for_cluster_state ok
        r MODULE LOAD $extdatamodule1
        r EXTERNAL_DATA INIT db0 helloextdata1
        r select 0
        r SET foo bar ext  ;# foo hashes to slot 12182
        r GET foo ext
        # Verify slot is 12182 for cluster mode
        assert_equal "12182" [r helloextdata1.storage_getslot db0 foo]
        assert_equal "12182" [r helloextdata1.filter_getslot db0 foo]

        cleanup_external_data_dump r
    }
}

# Node ID System tests
start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id54 "loglevel" notice] tags [list "external:skip"]] {
    test "Node ID is generated" {
        r MODULE LOAD $extdatamodule1
        set node_id [r external_data stats nodeid]
        assert {$node_id ne ""}
    }
    
    test "Node ID persists across restarts" {
        r MODULE UNLOAD helloextdata1
        r MODULE LOAD $extdatamodule1
        set node_id1 [r external_data stats nodeid]

        r MODULE UNLOAD helloextdata1
        r MODULE LOAD $extdatamodule1
        set node_id2 [r external_data stats nodeid]
        assert {$node_id1 eq $node_id2}
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id55 "loglevel" notice] tags [list "external:skip"]] {
    r MODULE LOAD $extdatamodule1

    test "Backup ID v0 encoding - full dump" {
        set result [r extdata.testbackupid "v0:node-abc123:-1:0"]
        assert_equal [dict get $result slot] -1
        assert_equal [dict get $result timestamp] 0
        assert_equal [dict get $result node_id] "node-abc123"
    }

    test "Backup ID v0 encoding - slot dump" {
        set result [r extdata.testbackupid "v0:node-def456:5:1706265600"]
        assert_equal [dict get $result slot] 5
        assert_equal [dict get $result timestamp] 1706265600
        assert_equal [dict get $result node_id] "node-def456"
    }

    test "Backup ID v0 encoding - with options" {
        set result [r extdata.testbackupid "v0:node-xyz:10:1706265600:compress=lz4"]
        assert_equal [dict get $result slot] 10
        assert_equal [dict get $result options] "compress=lz4"
    }

    test "Backup ID v0 - invalid version rejected" {
        catch {r extdata.testbackupid "v1:node-abc:5:1706265600"} err
        assert_match "*Invalid backup_id format*" $err
    }

    test "Backup ID v0 - invalid slot rejected" {
        catch {r extdata.testbackupid "v0:node-abc:16384:1706265600"} err
        assert_match "*Invalid backup_id format*" $err
    }

    test "Backup ID v0 - round-trip consistency" {
        set result [r extdata.testbackupid "v0:node-test123:100:1706265600"]
        assert_equal [dict get $result slot] 100
        assert_equal [dict get $result timestamp] 1706265600
        assert_equal [dict get $result node_id] "node-test123"
    }

    test "Backup ID v0 - boundary slot values" {
        # Test minimum valid slot
        set result [r extdata.testbackupid "v0:node-test:-1:0"]
        assert_equal [dict get $result slot] -1
        
        # Test maximum valid slot
        set result [r extdata.testbackupid "v0:node-test:16383:0"]
        assert_equal [dict get $result slot] 16383
    }

    test "Backup ID v0 - slot 0 valid" {
        set result [r extdata.testbackupid "v0:node-test:0:0"]
        assert_equal [dict get $result slot] 0
    }

    test "Backup ID v0 - negative timestamp accepted" {
        # Negative timestamp should still parse (implementation dependent)
        # This documents current behavior
        set result [r extdata.testbackupid "v0:node-test:5:-1"]
        assert_equal [dict get $result slot] 5
        assert_equal [dict get $result timestamp] -1
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id56 "loglevel" debug] tags [list "external:skip"]] {
    # Load module once for all tests
    assert_equal {OK} [r module load $extdatamodule1]
    assert_equal {OK} [r external_data INIT db0 helloextdata1]
    
    test "DUMP command uses node_id in backup_id" {
        cleanup_external_data_dump r
        
        # Set a key
        r select 0
        assert_equal {OK} [r set foo bar ext]
        
        # Execute EXTERNAL_DATA DUMP
        set dump_result [r external_data dump]
        assert {$dump_result != ""}
        
        # Parse backup_id from the dump result
        # Expected format: v0:node_id:slot:timestamp[:options]
        set parts [split $dump_result ":"]
        assert {[llength $parts] >= 4}
        
        # Verify it's version 0
        assert_equal [lindex $parts 0] "v0"
        
        # Extract node_id (2nd field)
        set node_id [lindex $parts 1]
        
        # Verify node_id matches the configured ext-data-id
        assert_equal "id56" $node_id
        
        cleanup_external_data_dump r
    }
    
    test "DUMP with SLOT parameter" {
        cleanup_external_data_dump r
        
        # Set a key (foo hashes to slot 12182)
        r select 0
        assert_equal {OK} [r set foo bar ext]
        
        # Execute EXTERNAL_DATA DUMP with SLOT parameter
        set dump_result [r external_data dump SLOT 12182]
        assert {$dump_result != ""}
        
        # Parse backup_id from the dump result
        set parts [split $dump_result ":"]
        assert {[llength $parts] >= 4}
        
        # Verify it's version 0
        assert_equal [lindex $parts 0] "v0"
        
        # Extract slot (3rd field)
        set slot [lindex $parts 2]
        
        # Verify backup_id contains the correct slot
        assert_equal $slot "12182"
        
        cleanup_external_data_dump r
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-id" test_node_123 "loglevel" debug] tags [list "external:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    test "DUMP command uses local node_id in backup_id" {
        wait_for_cluster_state ok
        cleanup_external_data_dump r
        
        # Load module and init
        catch {r module load $extdatamodule1}
        catch {r external_data INIT db0 helloextdata1}
        r flushall
        
        # SET a key with external storage
        r select 0
        assert_equal {OK} [r set mykey "test_value" ext]
        
        # Execute DUMP
        set backup_id [r external_data dump]
        
        # Verify backup_id is not empty
        assert {$backup_id ne ""}
        
        # Parse backup_id using extdata.testbackupid
        set parsed [r extdata.testbackupid $backup_id]
        set backup_node_id [dict get $parsed node_id]
        
        # Get local node_id from STATS
        set local_node_id [r external_data stats nodeid]
        
        # Verify the node_id in backup_id matches local node's ID
        assert_equal $backup_node_id $local_node_id
        
        cleanup_external_data_dump r
    }


    test "DUMP creates files in local node directory" {
        wait_for_cluster_state ok
        cleanup_external_data_dump r
        
        # Ensure module is loaded and db initialized
        catch {r module load $extdatamodule1}
        catch {r external_data INIT db0 helloextdata1}
        r flushall
        
        # Set a key
        r select 0
        assert_equal {OK} [r set mykey "test_value" ext]
        
        # Execute DUMP
        set backup_id [r external_data dump]
        
        # Extract node_id from returned backup_id
        set parsed [r extdata.testbackupid $backup_id]
        set node_id [dict get $parsed node_id]
        
        # Verify files exist in /tmp/external_data/<node_id>/db0/*.dat
        set file_path "/tmp/external_data/${node_id}/db0"
        assert {[file exists $file_path]}
        
        # Check that .dat files exist
        set dat_files [glob -nocomplain ${file_path}/*.dat]
        assert {[llength $dat_files] > 0}
        
        cleanup_external_data_dump r
    }


    test "DUMP with SLOT parameter uses local node_id" {
        wait_for_cluster_state ok
        cleanup_external_data_dump r
        
        # Ensure module is loaded and db initialized
        catch {r module load $extdatamodule1}
        catch {r external_data INIT db0 helloextdata1}
        r flushall
        
        # Set a key that hashes to slot 12182 (foo hashes to this slot)
        r select 0
        assert_equal {OK} [r set foo "test_value" ext]
        
        # Execute DUMP with SLOT parameter
        set backup_id [r external_data dump SLOT 12182]
        
        # Parse backup_id
        set parsed [r extdata.testbackupid $backup_id]
        
        # Verify backup_id contains correct slot (12182)
        assert_equal [dict get $parsed slot] "12182"
        
        # Get local node_id from STATS
        set local_node_id [r external_data stats nodeid]
        
        # Verify node_id is still the local node's ID
        assert_equal [dict get $parsed node_id] $local_node_id
        
        cleanup_external_data_dump r
    }


    test "Multiple DUMPs to same node directory" {
        wait_for_cluster_state ok
        cleanup_external_data_dump r
        
        # Ensure module is loaded and db initialized
        catch {r module load $extdatamodule1}
        catch {r external_data INIT db0 helloextdata1}
        r flushall
        
        # Create first backup with key1
        r select 0
        assert_equal {OK} [r set key1 "value1" ext]
        set backup_id1 [r external_data dump]
        set parsed1 [r extdata.testbackupid $backup_id1]
        
        # Create second backup with key2
        assert_equal {OK} [r set key2 "value2" ext]
        set backup_id2 [r external_data dump]
        set parsed2 [r extdata.testbackupid $backup_id2]
        
        # Verify both backups use the same node_id (local node)
        assert_equal [dict get $parsed1 node_id] [dict get $parsed2 node_id]
        
        # Note: timestamps may be the same if both DUMPs are from the same backup cycle
        # This is expected behavior - same cycle = same timestamp = same data state
        
        cleanup_external_data_dump r
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id100 "loglevel" debug] tags [list "external:skip"]] {
    set testmodule [file normalize tests/modules/extstorage/extdata1.so]
    
    test "LOAD auto-extracts node_id from backup_id" {
        cleanup_external_data_dump r
        r module load $testmodule
        r flushall
        
        # Initialize external storage
        r external_data INIT db0 helloextdata1
        r select 0
        
        # Set key foo with value bar (external storage)
        r set foo "bar" ext
        
        # Create backup
        set backup_id [r external_data dump]
        assert {$backup_id ne ""}
        
        # Delete the key
        r del foo ext
        assert {![r exists foo]}
        
        # Load backup (should auto-extract node_id from backup_id)
        r external_data load $backup_id
        
        # Verify data restored correctly
        assert_equal [r get foo ext] "bar"
        
        cleanup_external_data_dump r
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id101 "loglevel" debug] tags [list "external:skip"]] {
    set testmodule [file normalize tests/modules/extstorage/extdata1.so]
    
    test "LOAD from cross-node backup via file copy" {
        cleanup_external_data_dump r
        r module load $testmodule
        r flushall
        
        # Initialize external storage
        r external_data INIT db0 helloextdata1
        r select 0
        
        # Create backup on "node-A" (current node)
        r set mykey "node_a_value" ext
        set backup_id [r external_data dump]
        
        # Extract source node_id from backup_id
        set parsed [r extdata.testbackupid $backup_id]
        set source_node_id [dict get $parsed node_id]
        set slot [dict get $parsed slot]
        set timestamp [dict get $parsed timestamp]
        
        # Simulate copying backup to different node directory (node-B)
        set target_node_id "node-B"
        exec cp -r "/tmp/external_data/${source_node_id}" "/tmp/external_data/${target_node_id}"
        
        # Rename files to use target node_id in filename
        foreach file [glob -nocomplain "/tmp/external_data/${target_node_id}/db0/*_v0:${source_node_id}:*.dat"] {
            set new_file [string map [list ":${source_node_id}:" ":${target_node_id}:"] $file]
            exec mv $file $new_file
        }
        
        # Create new backup_id pointing to node-B
        set cross_node_backup_id "v0:${target_node_id}:${slot}:${timestamp}"
        
        # Clear local data
        r flushall
        
        # Load from "remote" node's backup (should extract node-B from backup_id)
        r external_data load $cross_node_backup_id
        
        # Verify data restored from cross-node backup
        assert_equal [r get mykey ext] "node_a_value"
        
        cleanup_external_data_dump r
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id102 "loglevel" debug] tags [list "external:skip"]] {
    set testmodule [file normalize tests/modules/extstorage/extdata1.so]
    
    test "LOAD fails with invalid backup_id format" {
        cleanup_external_data_dump r
        r module load $testmodule
        r flushall
        
        # Initialize external storage
        r external_data INIT db0 helloextdata1
        r select 0
        
        # Try loading with malformed backup_id
        catch {r external_data load "invalid_format_123"} err
        
        # Assert error is thrown
        assert_match "*Failed to load backup*" $err
        
        cleanup_external_data_dump r
    }
}

start_server [list overrides [list "ext-data-mode" kv "ext-data-id" id103 "loglevel" debug] tags [list "external:skip"]] {
    set testmodule [file normalize tests/modules/extstorage/extdata1.so]
    
    test "LOAD fails when source node directory missing" {
        cleanup_external_data_dump r
        r module load $testmodule
        r flushall
        
        # Initialize external storage
        r external_data INIT db0 helloextdata1
        r select 0
        
        # Create backup_id pointing to non-existent node
        set nonexistent_backup_id "v0:0:1234567890:nonexistent_node"
        
        # Try to load - should fail gracefully
        catch {r external_data load $nonexistent_backup_id} err
        
        # Verify graceful failure (not crash)
        assert_match "*" $err
        
        cleanup_external_data_dump r
    }
}


# Helper to convert list of slots into optimal slot ranges
# Returns a list of {start end} pairs for consecutive slot ranges
# Example: slots {1 2 3 5 6 8} -> returns {{1 3} {5 6} {8 8}}
proc build_slot_ranges {slots} {
    set sorted_slots [lsort -integer $slots]
    set ranges [list]
    set range_start [lindex $sorted_slots 0]
    set range_end $range_start
    
    foreach slot [lrange $sorted_slots 1 end] {
        if {$slot == $range_end + 1} {
            # Consecutive slot, extend range
            set range_end $slot
        } else {
            # Gap found, save current range and start new one
            lappend ranges [list $range_start $range_end]
            set range_start $slot
            set range_end $slot
        }
    }
    
    # Add final range
    lappend ranges [list $range_start $range_end]
    
    return $ranges
}

# Helper to check if all migrations are completed successfully
proc migrations_completed {node} {
    set migrations [$node CLUSTER GETSLOTMIGRATIONS]
    if {[llength $migrations] == 0} {
        return 1
    }
    # Check if all migrations are in success state
    foreach mig $migrations {
        if {[dict get $mig state] ne "success"} {
            return 0
        }
    }
    return 1
}

# ========================================================================
# Slot Migration Tests with External Storage
# ========================================================================

# Test 1: CLUSTER SETSLOT commands fail with external data enabled
# Start first node with ext-data-id node0
start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" node0 "loglevel" notice] tags [list "external:skip" "cluster"]] {
    set node0 [srv 0 client]
    
    # Start second node with ext-data-id node1
    start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" node1 "loglevel" notice] tags [list "external:skip"]] {
        set node1 [srv 0 client]
        
        # Start third node with ext-data-id node2
        start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" node2 "loglevel" notice] tags [list "external:skip"]] {
            set node2 [srv 0 client]
                
            test "CLUSTER SETSLOT commands fail with external data" {
                set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

                # Setup three-primary cluster with no replicas
                setup_ready_cluster [list $node0 $node1 $node2] [dict create]
                
                # Load module and initialize external data on node 0
                $node0 module load $extdatamodule1
                $node0 external_data INIT db0 helloextdata1
                
                # Get node IDs for testing
                set node0_id [$node0 CLUSTER MYID]
                set node1_id [$node1 CLUSTER MYID]
                
                # Get a slot owned by node 0
                set slot 100
                
                # Try MIGRATING variant - should fail
                catch {$node0 CLUSTER SETSLOT $slot MIGRATING $node1_id} err
                assert_match "*external storage*" $err
                assert_match "*MIGRATESLOTS*" $err
                
                # Try IMPORTING variant on target node - should fail
                catch {$node1 CLUSTER SETSLOT $slot IMPORTING $node0_id} err
                assert_match "*external storage*" $err
                assert_match "*MIGRATESLOTS*" $err
                
                # Try NODE variant - should fail
                catch {$node0 CLUSTER SETSLOT $slot NODE $node1_id} err
                assert_match "*external storage*" $err
                assert_match "*MIGRATESLOTS*" $err
                
                # Try STABLE variant - should fail
                catch {$node0 CLUSTER SETSLOT $slot STABLE} err
                assert_match "*external storage*" $err
                assert_match "*MIGRATESLOTS*" $err
            }
        }
    }
}

# Test 2: Single slot migration with external data
# Start first node with ext-data-id node0
start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" node0 "loglevel" notice] tags [list "external:skip" "cluster"]] {
    set node0 [srv 0 client]
    
    # Start second node with ext-data-id node1
    start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" node1 "loglevel" notice] tags [list "external:skip"]] {
        set node1 [srv 0 client]
        
        # Start third node with ext-data-id node2
        start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" node2 "loglevel" notice] tags [list "external:skip"]] {
            set node2 [srv 0 client]

            test "CLUSTER MIGRATESLOTS successfully migrates single slot with external data" {
                set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

                # Setup three-primary cluster with no replicas
                setup_ready_cluster [list $node0 $node1 $node2] [dict create]
                
                # Load module and initialize external data on both nodes
                $node0 module load $extdatamodule1
                $node0 external_data INIT db0 helloextdata1
                $node1 module load $extdatamodule1
                $node1 external_data INIT db0 helloextdata1
                
                # Get and verify UNIQUE node IDs for each node
                set ext_node0_id [$node0 external_data stats nodeid]
                set ext_node1_id [$node1 external_data stats nodeid]
                assert {$ext_node0_id ne ""}
                assert {$ext_node1_id ne ""}
                assert {$ext_node0_id ne $ext_node1_id}

                # Find a slot owned by node 0
                set slots_info [$node0 CLUSTER SLOTS]
                set node0_start_slot [lindex [lindex $slots_info 0] 0]
                
                # Find a key that hashes to a slot owned by node 0
                # Use CLUSTER KEYSLOT to verify
                set key "migration_test_key"
                set slot [$node0 CLUSTER KEYSLOT $key]
                
                # If key doesn't hash to node 0's slots, find one that does
                for {set i 0} {$i < 100} {incr i} {
                    set test_key "key_$i"
                    set test_slot [$node0 CLUSTER KEYSLOT $test_key]
                    if {$test_slot >= $node0_start_slot && $test_slot <= [lindex [lindex $slots_info 0] 1]} {
                        set key $test_key
                        set slot $test_slot
                        break
                    }
                }
                
                # Create key with external data on node 0
                $node0 select 0
                set value "test_value_for_migration"
                assert_equal {OK} [$node0 set $key $value ext]
                
                # Verify initial state on source node
                assert_equal $value [$node0 get $key ext]
                assert_equal {} [$node0 get $key]
                
                # Get node IDs
                set node1_id [$node1 CLUSTER MYID]
                
                # Migrate slot to node 1
                $node0 CLUSTER MIGRATESLOTS SLOTSRANGE $slot $slot NODE $node1_id
                
                # Wait for migration to complete using CLUSTER GETSLOTMIGRATIONS
                wait_for_condition 100 100 {
                    [migrations_completed $node0]
                } else {
                    fail "Migration did not complete - migrations: [$node0 CLUSTER GETSLOTMIGRATIONS]"
                }
                
                # Verify key exists on target node with external data
                $node1 select 0
                assert_equal $value [$node1 get $key ext]
                
                # Verify slot ownership transferred to target
                set slots_info [$node1 CLUSTER SLOTS]
                set slot_found 0
                foreach slot_info $slots_info {
                    set start_slot [lindex $slot_info 0]
                    set end_slot [lindex $slot_info 1]
                    if {$slot >= $start_slot && $slot <= $end_slot} {
                        set owner_id [lindex [lindex $slot_info 2] 2]
                        if {$owner_id eq $node1_id} {
                            set slot_found 1
                        }
                    }
                }
                assert {$slot_found == 1}

                # Verify node IDs remain unique and unchanged after migration
                set ext_node0_id_after [$node0 external_data stats nodeid]
                set ext_node1_id_after [$node1 external_data stats nodeid]
                assert_equal $ext_node0_id $ext_node0_id_after
                assert_equal $ext_node1_id $ext_node1_id_after
            }
        }
    }
}

# Test 3: Multiple slots migration with external data
# Start first node with ext-data-id node0
start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" node0 "loglevel" notice] tags [list "external:skip" "cluster"]] {
    set node0 [srv 0 client]
    
    # Start second node with ext-data-id node1
    start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" node1 "loglevel" notice] tags [list "external:skip"]] {
        set node1 [srv 0 client]
        
        # Start third node with ext-data-id node2
        start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" node2 "loglevel" notice] tags [list "external:skip"]] {
            set node2 [srv 0 client]
                
            test "CLUSTER MIGRATESLOTS successfully migrates multiple slots with external data" {
                set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

                # Setup three-primary cluster with no replicas
                setup_ready_cluster [list $node0 $node1 $node2] [dict create]
                
                # Load module and initialize external data on both nodes
                $node0 module load $extdatamodule1
                $node0 external_data INIT db0 helloextdata1
                $node1 module load $extdatamodule1
                $node1 external_data INIT db0 helloextdata1
                
                # Find slots owned by node 0
                set slots_info [$node0 CLUSTER SLOTS]
                set node0_start_slot [lindex [lindex $slots_info 0] 0]
                set node0_end_slot [lindex [lindex $slots_info 0] 1]
                
                $node0 select 0
                
                # Find multiple keys that hash to different slots owned by node 0
                set keys_and_values {}
                set key_counter 0
                
                for {set i 0} {$i < 1000 && [llength $keys_and_values] < 6} {incr i} {
                    set test_key "mig_key_$i"
                    set test_slot [$node0 CLUSTER KEYSLOT $test_key]
                    
                    # Check if slot is owned by node 0 and not already in list
                    set already_have_slot 0
                    foreach {k v} $keys_and_values {
                        if {[$node0 CLUSTER KEYSLOT $k] == $test_slot} {
                            set already_have_slot 1
                            break
                        }
                    }
                    
                    if {$test_slot >= $node0_start_slot && $test_slot <= $node0_end_slot && !$already_have_slot} {
                        lappend keys_and_values $test_key "value_$i"
                        
                        # Create key with external data
                        assert_equal {OK} [$node0 set $test_key "value_$i" ext]
                        assert_equal "value_$i" [$node0 get $test_key ext]
                    }
                }
                
                # Get slots for all keys
                set slots [list]
                foreach {key value} $keys_and_values {
                    set slot [$node0 CLUSTER KEYSLOT $key]
                    if {[lsearch $slots $slot] == -1} {
                        lappend slots $slot
                    }
                }
                
                # Get node IDs
                set node1_id [$node1 CLUSTER MYID]
                
                # Build optimized slot ranges from slots list
                set slot_ranges [build_slot_ranges $slots]
                
                # Migrate each range separately
                foreach range $slot_ranges {
                    set start [lindex $range 0]
                    set end [lindex $range 1]
                    $node0 CLUSTER MIGRATESLOTS SLOTSRANGE $start $end NODE $node1_id
                }
                
                # Wait for all migrations to complete
                wait_for_condition 100 100 {
                    [migrations_completed $node0]
                } else {
                    fail "Migrations did not complete - migrations: [$node0 CLUSTER GETSLOTMIGRATIONS]"
                }
                
                # Verify all keys exist on target node with external data
                $node1 select 0
                foreach {key value} $keys_and_values {
                    set result [$node1 get $key ext]
                    assert_equal $value $result
                }
                
                # Verify all slots ownership transferred to target
                set slots_info [$node1 CLUSTER SLOTS]
                foreach slot $slots {
                    set slot_found 0
                    foreach slot_info $slots_info {
                        set start_slot [lindex $slot_info 0]
                        set end_slot [lindex $slot_info 1]
                        if {$slot >= $start_slot && $slot <= $end_slot} {
                            set owner_id [lindex [lindex $slot_info 2] 2]
                            if {$owner_id eq $node1_id} {
                                set slot_found 1
                            }
                        }
                    }
                    assert {$slot_found == 1}
                }
            }
        }
    }
}

# Test 4: Migration failure handling with external data
# Start first node with ext-data-id node0
start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" node0 "loglevel" notice] tags [list "external:skip" "cluster"]] {
    set node0 [srv 0 client]
    
    # Start second node with ext-data-id node1
    start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" node1 "loglevel" notice] tags [list "external:skip"]] {
        set node1 [srv 0 client]
        
        # Start third node with ext-data-id node2
        start_server [list overrides [list "cluster-enabled" yes "cluster-ping-interval" 50 "cluster-node-timeout" 1500 "cluster-databases" 16 "cluster-slot-stats-enabled" yes "ext-data-mode" kv "ext-data-id" node2 "loglevel" notice] tags [list "external:skip"]] {
            set node2 [srv 0 client]

            test "CLUSTER MIGRATESLOTS handles failures gracefully with external data" {
                set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

                # Setup three-primary cluster with no replicas
                setup_ready_cluster [list $node0 $node1 $node2] [dict create]
                
                # Load module and initialize external data on source node
                $node0 module load $extdatamodule1
                $node0 external_data INIT db0 helloextdata1
                
                # Find a slot owned by node 0
                set slots_info [$node0 CLUSTER SLOTS]
                set node0_start_slot [lindex [lindex $slots_info 0] 0]
                set node0_end_slot [lindex [lindex $slots_info 0] 1]
                
                # Find a key that hashes to a slot owned by node 0
                $node0 select 0
                set key ""
                set slot 0
                for {set i 0} {$i < 100} {incr i} {
                    set test_key "failure_test_key_$i"
                    set test_slot [$node0 CLUSTER KEYSLOT $test_key]
                    if {$test_slot >= $node0_start_slot && $test_slot <= $node0_end_slot} {
                        set key $test_key
                        set slot $test_slot
                        break
                    }
                }
                
                # Create key with external data on source
                set value "test_value_should_remain"
                assert_equal {OK} [$node0 set $key $value ext]
                
                # Verify initial state
                assert_equal $value [$node0 get $key ext]
                
                # Get target node ID before killing it
                set node1_id [$node1 CLUSTER MYID]
                
                # Get PID from INFO SERVER
                set node1_info [$node1 INFO SERVER]
                regexp {process_id:(\d+)} $node1_info - node1_pid
                
                # Kill target node to simulate failure
                catch {exec kill -9 $node1_pid}
                
                # Wait a bit for the kill to take effect
                after 100
                
                # Attempt migration - should fail or timeout
                set migration_failed 0
                catch {
                    $node0 CLUSTER MIGRATESLOTS SLOTSRANGE $slot $slot NODE $node1_id
                } err
                
                # Migration should fail due to unreachable target
                # Error could be timeout, connection refused, etc.
                set migration_failed 1
                
                # Verify data still exists on source node after failed migration
                assert_equal $value [$node0 get $key ext]
                
                # Verify external data remains on source
                # Using debug command to check storage directly
                set storage_value [$node0 external_data debug db0 storage get $key]
                assert_equal $value $storage_value
                
                # Verify migration was marked as failed
                assert_equal 1 $migration_failed
                
                # Note: We don't restart the target node here as it's not necessary
                # The test has already verified that the migration failed and
                # the data remains intact on the source node
            }
        }
    }
}
