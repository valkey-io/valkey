set someothermodule [file normalize tests/modules/timer.so]
set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
set extdatamodule2 [file normalize tests/modules/extstorage/extdata2.so]
set ext_data_off_err "ERR External data commands are unavailable with ext-data-mode off"

start_server {tags {"external_data external:skip"}} {
    test {Running EXTERNAL_DATA commands with switched off external data fails} {
        assert_error $ext_data_off_err {r external_data init db0 helloextdata1}
        assert_error $ext_data_off_err {r external_data loaded}
        assert_error $ext_data_off_err {r external_data stats}
        assert_error $ext_data_off_err {r set k v ext}
    }
}

start_server [list overrides [list "ext-data-mode" kv] tags [list "external:skip"]] {
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

start_server [list overrides [list "ext-data-mode" kv] tags [list "external:skip"]] {
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
start_server [list overrides [list "ext-data-mode" kv] tags [list "external:skip"]] {
    test {Initializing and dropping db affects STATS commands} {
        # STATS ok with modules non-loaded
        assert_equal [list ] [r external_data stats]

        # check init input arguments
        assert_error {ERR failed to parse db number from db, expect db0, db10, etc.} {r external_data INIT db helloextdata1}
        assert_error {ERR db number 16 exceeds used on server 0-15} {r external_data INIT db16 helloextdata1}
        assert_error {ERR wrong number of arguments for 'external_data|init' command} {r external_data INIT db0 }

        # you need to load modules to init
        assert_error {ERR module helloextdata1 is not loaded} {r external_data INIT db0 helloextdata1}

        # STATS ok with modules loaded
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal [list ] [r external_data stats]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal [list db0:helloextdata1] [r external_data stats]

        # Load same module again after init fails, but the server won't crash
        assert_error {ERR Error loading the extension. Please check the server logs.} {r module load $extdatamodule1}

        # Another module is added to stats
        assert_equal {OK} [r module load $extdatamodule2]
        assert_equal {OK} [r external_data INIT db1 helloextdata2]
        assert_equal [list db0:helloextdata1 db1:helloextdata2] [r external_data stats]

        # you can't init the same db without dropping its currently used modules
        assert_error {ERR db0 is already initialized} {r external_data INIT db0 helloextdata1}

        # unload loaded and inited module fails
        assert_error {ERR Error unloading module: operation not possible.} {r module unload helloextdata1}

        # dropping succeeds
        assert_error {ERR Leads to persistent storage data loss for db0, use FORCE if sure} {r external_data drop db0}
        assert_equal {OK} [r external_data drop db0 FORCE]
        assert_equal [list db1:helloextdata2] [r external_data stats]
        assert_equal {OK} [r module unload helloextdata1]

        # init again succeeds
        assert_equal {OK} [r external_data INIT db0 helloextdata2]
        assert_equal [list db0:helloextdata2 db1:helloextdata2] [r external_data stats]

        # cleanup ok
        assert_equal {OK} [r external_data drop db0 FORCE]
        assert_equal {OK} [r external_data drop db1 FORCE]
        assert_equal [list ] [r external_data stats]
        assert_equal {OK} [r module unload helloextdata2]
    }
}

start_server [list overrides [list "ext-data-mode" kv "loglevel" debug "ext-data-timeout" 0] tags [list "external:skip" "singledb:skip"]] {
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

start_server [list overrides [list "ext-data-mode" kv "loglevel" debug "ext-data-timeout" 0] tags [list "external:skip" "singledb:skip"]] {
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

start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
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

start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
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

start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
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
start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
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

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
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

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
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

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
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
start_server [list overrides [list "ext-data-mode" kv "loglevel" debug "ext-data-store-by-size" "0"] tags [list "external:skip"]] {
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

start_server [list overrides [list "ext-data-mode" kv "loglevel" debug "ext-data-store-by-size" "0"] tags [list "external:skip" "singledb:skip"]] {
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
start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-store-by-size" "0"]] {
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

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "ext-data-store-by-size" "0"] tags [list "singledb:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    test "DEL with EXT - Sharded - Different databases" {
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
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

start_server [list overrides [list "ext-data-mode" kv "loglevel" debug "ext-data-timeout" 5] tags [list "external:skip" "block"]] {
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
start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
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

start_server [list overrides [list "ext-data-mode" kv "loglevel" debug "maxmemory-policy" "allkeys-lru" "ext-data-expire" "yes"] tags [list "external:skip" "slow"]] {
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

start_server [list overrides [list "ext-data-mode" kv "loglevel" debug "maxmemory-policy" "volatile-lru" "ext-data-expire" "yes"] tags [list "external:skip" "slow"]] {
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
start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
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
start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
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
start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
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
start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
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

start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "slow" "singledb:skip"]] {
    test {Performance with large dataset (should complete in under 500ms)} {
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r external_data INIT db1 helloextdata1]
        r select 0
        
        # Insert a large number of keys into db0 external storage
        # Using 100,000 keys to demonstrate the O(n) performance issue
        set num_keys 100000
        puts "Inserting $num_keys keys into db0 external storage..."
        
        set insert_start [clock milliseconds]
        for {set i 0} {$i < $num_keys} {incr i} {
            r set perf_key_$i perf_value_$i ext
        }
        set insert_end [clock milliseconds]
        set insert_time [expr {$insert_end - $insert_start}]
        puts "Insert time: ${insert_time}ms for $num_keys keys"
        
        # Before flushall create a backup
        puts "Creating backup before flushall..."
        set backup_result [r external_data dump]
        assert {$backup_result != ""}
        
        # Insert just one key into db1 to see the difference
        r select 1
        assert_equal {OK} [r set single_key single_value ext]
        puts "Inserted 1 key into db1 external storage"
        
        # Verify keys exist in both databases
        r select 0
        assert_equal perf_value_0 [r get perf_key_0 ext]
        assert_equal perf_value_[expr {$num_keys - 1}] [r get perf_key_[expr {$num_keys - 1}] ext]
        r select 1
        assert_equal single_value [r get single_key ext]
        
        # Test DBSIZE with EXT flag performance
        puts "Testing DBSIZE with EXT flag performance..."
        r select 0
        
        # Measure DBSIZE with EXT performance
        puts "Executing DBSIZE EXT..."
        set dbsize_start [clock milliseconds]
        set dbsize_result [r dbsize ext]
        set dbsize_end [clock milliseconds]
        set dbsize_time [expr {$dbsize_end - $dbsize_start}]
        
        puts "DBSIZE EXT time: ${dbsize_time}ms for $num_keys keys"
        puts "DBSIZE EXT result: $dbsize_result"
        
        # Verify DBSIZE EXT returns the correct count
        assert_equal $num_keys $dbsize_result
        
        # Assert DBSIZE EXT completes in under 500ms
        if {$dbsize_time >= 500} {
            fail "DBSIZE EXT took ${dbsize_time}ms, expected < 500ms"
        }
        
        # First test SWAPDB performance with large dataset
        puts "Testing SWAPDB performance with large dataset..."
        
        # Measure SWAPDB performance
        puts "Executing SWAPDB..."
        set swap_start [clock milliseconds]
        assert_equal {OK} [r swapdb 0 1]
        set swap_end [clock milliseconds]
        set swap_time [expr {$swap_end - $swap_start}]
        
        puts "SWAPDB time: ${swap_time}ms for $num_keys keys"
        
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
        puts "Testing FLUSHDB performance with large dataset..."
        r select 1
        
        # Measure FLUSHDB performance
        puts "Executing FLUSHDB..."
        set flush_start [clock milliseconds]
        assert_equal {OK} [r flushdb]
        set flush_end [clock milliseconds]
        set flush_time [expr {$flush_end - $flush_start}]
        
        puts "FLUSHDB time: ${flush_time}ms for $num_keys keys"
        
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
        puts "Restoring from backup after flushall..."
        r select 0
        r external_data load $backup_result
        
        # Ensure that DBSIZE is the correct one without iterating through keys (too slow)
        set dbsize_after_restore [r dbsize ext]
        assert_equal $num_keys $dbsize_after_restore
        puts "DBSIZE after restore: $dbsize_after_restore (expected: $num_keys)"
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "slow" "singledb:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
    
    test {Performance with large dataset (should complete in under 500ms) (cluster mode)} {
        wait_for_cluster_state ok
        
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r external_data INIT db1 helloextdata1]
        r select 0
        
        # Insert a large number of keys into db0 external storage
        # Using 100,000 keys to demonstrate the O(n) performance issue
        set num_keys 100000
        puts "Inserting $num_keys keys into db0 external storage in cluster mode..."
        
        set insert_start [clock milliseconds]
        for {set i 0} {$i < $num_keys} {incr i} {
            r set perf_key_$i perf_value_$i ext
        }
        set insert_end [clock milliseconds]
        set insert_time [expr {$insert_end - $insert_start}]
        puts "Insert time: ${insert_time}ms for $num_keys keys"
        
        # Before flushall create a backup
        puts "Creating backup before flushall..."
        set backup_result [r external_data dump]
        assert {$backup_result != ""}

        # Insert just one key into db1 to see the difference
        r select 1
        assert_equal {OK} [r set single_key single_value ext]
        puts "Inserted 1 key into db1 external storage"
        
        # Verify keys exist in both databases
        r select 0
        assert_equal perf_value_0 [r get perf_key_0 ext]
        assert_equal perf_value_[expr {$num_keys - 1}] [r get perf_key_[expr {$num_keys - 1}] ext]
        r select 1
        assert_equal single_value [r get single_key ext]
        
        # Test DBSIZE with EXT flag performance in cluster mode
        puts "Testing DBSIZE with EXT flag performance in cluster mode..."
        r select 0
        
        # Measure DBSIZE with EXT performance
        puts "Executing DBSIZE EXT in cluster mode..."
        set dbsize_start [clock milliseconds]
        set dbsize_result [r dbsize ext]
        set dbsize_end [clock milliseconds]
        set dbsize_time [expr {$dbsize_end - $dbsize_start}]
        
        puts "DBSIZE EXT time: ${dbsize_time}ms for $num_keys keys in cluster mode"
        puts "DBSIZE EXT result: $dbsize_result"
        
        # Verify DBSIZE EXT returns the correct count
        assert_equal $num_keys $dbsize_result
        
        # Assert DBSIZE EXT completes in under 500ms
        if {$dbsize_time >= 500} {
            fail "DBSIZE EXT took ${dbsize_time}ms, expected < 500ms in cluster mode"
        }
        
        # Note: SWAPDB is not allowed in cluster mode, so we skip that test
        puts "SWAPDB is not allowed in cluster mode, skipping SWAPDB performance test"
        
        # Now test FLUSHDB performance with the large dataset (in db0)
        puts "Testing FLUSHDB performance with large dataset in cluster mode..."
        r select 0
        
        # Measure FLUSHDB performance
        puts "Executing FLUSHDB in cluster mode..."
        set flush_start [clock milliseconds]
        assert_equal {OK} [r flushdb]
        set flush_end [clock milliseconds]
        set flush_time [expr {$flush_end - $flush_start}]
        
        puts "FLUSHDB time: ${flush_time}ms for $num_keys keys in cluster mode"
        
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
        puts "Restoring from backup after flushall..."
        r select 0
        r external_data load $backup_result
        
        # Ensure that DBSIZE is the correct one without iterating through keys (too slow)
        set dbsize_after_restore [r dbsize ext]
        assert_equal $num_keys $dbsize_after_restore
        puts "DBSIZE after restore: $dbsize_after_restore (expected: $num_keys)"
    }
}

# Test SWAPDB command with external storage
# SWAPDB should swap external data between two databases
start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
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
start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
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
    
start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
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
start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
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

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
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
start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
    test "EXTERNAL_DATA DUMP and LOAD: Basic functionality" {
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
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
    }

    test "EXTERNAL_DATA DUMP and LOAD: With AOF enabled (persistence auto-load)" {
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
        
        # Restart server to test auto-load on startup
        restart_server 0 true false
        wait_done_loading r
        
        # Verify external data is automatically restored on startup
        assert_equal "value1" [r get key1 ext]
        assert_equal "value2" [r get key2 ext]

        # Cleanup
        r config set appendonly no
        r config rewrite
        r flushall
    }

    test "EXTERNAL_DATA DUMP and LOAD: With save enabled (persistence auto-load)" {
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
        
        # Restart server to test auto-load on startup
        restart_server 0 true false
        wait_done_loading r
        
        # Verify external data is automatically restored on startup
        assert_equal "value1" [r get key1 ext]
        assert_equal "value2" [r get key2 ext]

        # Cleanup
        r config set save ""
        r config rewrite
        r flushall
    }

    test "EXTERNAL_DATA DUMP and LOAD: With both AOF and save disabled (no persistence)" {
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
        
        # Restart server - should NOT auto-load without persistence
        restart_server 0 true false
        wait_done_loading r
        
        # Verify external data is NOT restored without persistence
        assert_equal "" [r get key1 ext]
        assert_equal "" [r get key2 ext]

        # Cleanup
        r flushall
    }

    test "EXTERNAL_DATA DUMP: Empty database" {
        # Dump external data from empty database - should succeed without error
        set empty_dump [r external_data dump]
        assert {$empty_dump != ""}

        # Check that server is ok, ext is processed
        r select 0
        assert_equal "" [r get key1]
        assert_equal "" [r get key1 ext]
        r set key1 "value1" EXT
        assert_equal "value1" [r get key1 ext]
    }

    test "EXTERNAL_DATA LOAD: Invalid dump data" {
        # Try to load invalid dump data
        catch { r external_data load "invalid_data" } result
        assert_match "*ERR*" $result

        # Check that server is ok, ext is processed
        r select 0
        assert_equal "" [r get key1]
        assert_equal "" [r get key1 ext]
        r set key1 "value1" EXT
        assert_equal "value1" [r get key1 ext]
    }

    test "EXTERNAL_DATA DUMP and LOAD: Large dataset" {
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
    }
}

start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
    test "EXTERNAL_DATA DUMP and LOAD: Multiple databases" {
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]

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
        assert_equal [r get db0_key] "db0_value"
        r select 1
        assert_equal [r get db1_key ext] "db1_value"
        assert_equal [r get db1_key_2 ext] ""
    }
}

test "EXTERNAL_DATA DUMP and LOAD: Partial sync scenario" {
    start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
        start_server [list overrides [list "ext-data-mode" kv "loglevel" debug]] {
            set primary [srv -1 client]
            set primary_host [srv -1 host]
            set primary_port [srv -1 port]
            set replica [srv 0 client]

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
                [lindex [$replica role] 0] eq {replica} &&
                [lindex [$primary role] 1] eq {1}
            } else {
                fail "Replication not started."
            }
            
            # Init external data for replica now
            assert_equal {OK} [$replica module load $extdatamodule1]
            assert_equal {OK} [$replica external_data INIT db0 helloextdata1]

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
        }
    }
}

test "EXTERNAL_DATA DUMP and LOAD: replication" {
    start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
        start_server [list overrides [list "ext-data-mode" kv "loglevel" debug]] {
            # Get primary and replica nodes
            set primary [srv 0 client]
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]
            set replica [srv -1 client]

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
                [lindex [$replica role] 0] eq {replica} &&
                [lindex [$primary role] 1] eq {1}
            } else {
                fail "Replication not started."
            }

            # Replica has no external data as module is not loaded, server is not failing
            assert_equal [$replica get memory_key] "value"
            assert_error $ext_data_off_err {$replica get primary_key1 ext}
            assert_error $ext_data_off_err {$replica get primary_key2 ext}

            # Init external data for replica now
            assert_equal {OK} [$replica module load $extdatamodule1]
            assert_equal {OK} [$replica external_data INIT db0 helloextdata1]

            # Setup replication and wait for replica to sync initial external data
            $replica replicaof $primary $primary_port
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
            assert_equal [$replica get primary_key3 ext] ""
        }
    }
}

# Cluster mode tests for EXTERNAL_DATA DUMP and LOAD commands
start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "singledb:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
    
    test "EXTERNAL_DATA DUMP and LOAD: Basic functionality (cluster mode)" {
        wait_for_cluster_state ok
        
        # Load module and initialize external storage
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
    }

    test "EXTERNAL_DATA DUMP and LOAD: With AOF enabled (cluster mode, persistence auto-load)" {
        wait_for_cluster_state ok
        
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
        
        # Verify external data is automatically restored on startup
        assert_equal "value1" [r get key1 ext]
        assert_equal "value2" [r get key2 ext]

        # Cleanup
        r config set appendonly no
        r config rewrite
        r flushall
    }

    test "EXTERNAL_DATA DUMP and LOAD: With save enabled (cluster mode, persistence auto-load)" {
        wait_for_cluster_state ok
        
        # Enable save persistence
        r select 0
        r config set save "900 1"
        r cofnig rewrite
        
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
        
        # Verify external data is automatically restored on startup
        assert_equal "value1" [r get key1 ext]
        assert_equal "value2" [r get key2 ext]

        # Cleanup
        r config set save ""
        r config rewrite
        r flushall
    }

    test "EXTERNAL_DATA DUMP and LOAD: With both AOF and save disabled (cluster mode, no persistence)" {
        wait_for_cluster_state ok
        
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
        
        # Verify external data is NOT restored without persistence
        assert_equal "" [r get key1 ext]
        assert_equal "" [r get key2 ext]
    }

    test "EXTERNAL_DATA DUMP: Empty database (cluster mode)" {
        wait_for_cluster_state ok
        
        # Dump external data from empty database - should succeed without error
        set empty_dump [r external_data dump]
        assert {$empty_dump != ""}

        # Check that server is ok, ext is processed
        r select 0
        assert_equal "" [r get key1]
        assert_equal "" [r get key1 ext]
        r set key1 "value1" EXT
        assert_equal "value1" [r get key1 ext]
    }

    test "EXTERNAL_DATA LOAD: Invalid dump data (cluster mode)" {
        wait_for_cluster_state ok
        
        # Try to load invalid dump data
        catch { r external_data load "invalid_data" } result
        assert_match "*ERR*" $result

        # Check that server is ok, ext is processed
        r select 0
        assert_equal "" [r get key1]
        assert_equal "" [r get key1 ext]
        r set key1 "value1" EXT
        assert_equal "value1" [r get key1 ext]
    }

    test "EXTERNAL_DATA DUMP and LOAD: Large dataset (cluster mode)" {
        wait_for_cluster_state ok
        
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
    }

    test "EXTERNAL_DATA DUMP and LOAD: Multiple databases (cluster mode)" {
        wait_for_cluster_state ok
        
        # Load module and initialize external storage
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
        assert_equal [r get db0_key] "db0_value"
        r select 1
        assert_equal [r get db1_key ext] "db1_value"
        assert_equal [r get db1_key_2 ext] ""
    }
}

test "EXTERNAL_DATA DUMP and LOAD: Partial sync scenario (cluster mode)" {
    start_cluster 1 1 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
        set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
        
        wait_for_cluster_state ok
        
        # Get primary and replica nodes
        set primary [srv 0 client]
        set replica [srv -1 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        
        # Load module and initialize external storage on primary
        assert_equal {OK} [$primary module load $extdatamodule1]
        assert_equal {OK} [$primary external_data INIT db0 helloextdata1]

        # Configure replication backlog for partial sync
        $primary config set repl-backlog-size 1024
        $primary config set repl-backlog-ttl 3600
        
        # Add some external data on primary
        $primary select 0
        $primary set memory_key value
        $primary set cluster_key1 "cluster_value1" EXT
        $primary set cluster_key2 "cluster_value2" EXT

        # Init external data for replica now
        assert_equal {OK} [$replica module load $extdatamodule1]
        assert_equal {OK} [$replica external_data INIT db0 helloextdata1]
        
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
        
        # Reconnect replica
        $replica replicaof $primary_host $primary_port
        
        # Wait for partial sync to complete
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
    }
}

test "EXTERNAL_DATA DUMP and LOAD: replication (cluster mode)" {
    start_cluster 1 1 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
        set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
        
        wait_for_cluster_state ok
        
        # Get primary and replica nodes
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        set replica [srv -1 client]

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

        # Replica has no external data as module is not loaded, server is not failing
        assert_equal [$replica get memory_key] "value"
        assert_error $ext_data_off_err {$replica get primary_key1 ext}
        assert_error $ext_data_off_err {$replica get primary_key2 ext}

        # Init external data for replica now
        assert_equal {OK} [$replica module load $extdatamodule1]
        assert_equal {OK} [$replica external_data INIT db0 helloextdata1]

        # Wait for replica to sync initial external data
        $replica replicaof $primary $primary_port
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
        assert_equal [$replica get primary_key3 ext] ""
    }
}


# Test async external data dump for replication
start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "slow"]] {
    test "Async external data dump: Basic functionality" {
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
    }
}

test "Async external data dump: Non-blocking behavior" {
    start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
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
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
    
    test "Async external data dump: Cluster mode basic functionality" {
        wait_for_cluster_state ok
        
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
    }
    
    test "Async external data dump: Non-blocking behavior (cluster mode)" {
        wait_for_cluster_state ok
        
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
    }
}

# Test ext-data-async-load configuration
test "ext-data-async-load: Sync mode" {
    start_server [list overrides [list "ext-data-mode" kv "loglevel" debug "ext-data-async-load" no] tags [list "external:skip"]] {
        start_server [list overrides [list "ext-data-mode" kv "loglevel" debug "ext-data-async-load" no]] {
            set primary [srv -1 client]
            set primary_host [srv -1 host]
            set primary_port [srv -1 port]
            set replica [srv 0 client]
            
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
            wait_for_condition 50 100 {
                [lindex [$replica role] 0] eq {replica}
            } else {
                fail "Replication not started"
            }
            
            # Load module on replica
            assert_equal {OK} [$replica module load $extdatamodule1]
            assert_equal {OK} [$replica external_data INIT db0 helloextdata1]
            
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
        }
    }
}

test "ext-data-async-load: Sync mode (cluster)" {
    start_cluster 1 1 [list overrides [list "ext-data-mode" kv "loglevel" debug "ext-data-async-load" no] tags [list "external:skip"]] {
        set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
        
        wait_for_cluster_state ok
        
        set primary [srv 0 client]
        set replica [srv -1 client]
        
        # Verify ext-data-async-load is disabled
        set config_result [$replica config get ext-data-async-load]
        assert_equal "no" [lindex $config_result 1]
        
        # Load module and initialize external storage on primary
        assert_equal {OK} [$primary module load $extdatamodule1]
        assert_equal {OK} [$primary external_data INIT db0 helloextdata1]
        
        # Add external data on primary
        $primary select 0
        $primary set cluster_sync_key1 "cluster_sync_value1" EXT
        $primary set cluster_sync_key2 "cluster_sync_value2" EXT
        $primary set cluster_memory_key "cluster_memory_value"
        
        # Load module on replica
        assert_equal {OK} [$replica module load $extdatamodule1]
        assert_equal {OK} [$replica external_data INIT db0 helloextdata1]
        
        # In sync mode, both memory and external data should be available together
        wait_for_condition 50 100 {
            [$replica get cluster_memory_key] eq "cluster_memory_value" &&
            [$replica get cluster_sync_key1 ext] eq "cluster_sync_value1" &&
            [$replica get cluster_sync_key2 ext] eq "cluster_sync_value2"
        } else {
            fail "Data not synced in sync mode (cluster)"
        }
        
        # Check replica logs for sync load message
        set replica_log [srv -1 stdout]
        set replica_log_content [exec cat $replica_log]
        assert_match "*loaded synchronously*" $replica_log_content
    }
}


# Test external storage with simulated failures
# Tests both standalone and cluster mode with 50% failure rate
start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
    test "External storage with simulated failures - standalone mode" {
        # Load module with 50% failure rate (fails every 2nd set, starting from 1st)
        set extdatamodule1_with_failures [file normalize tests/modules/extstorage/extdata1.so]
        assert_equal {OK} [r module load $extdatamodule1_with_failures set_failure_percent=50]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        r select 0
        
        # First set should fail (1st operation)
        assert_error "*Simulated storage failure*" {r set key1 value1 ext}
        
        # Second set should succeed (2nd operation)
        assert_equal {OK} [r set key2 value2 ext]
        
        # Third set should fail (3rd operation)
        assert_error "*Simulated storage failure*" {r set key3 value3 ext}
        
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
test "External storage replication with simulated failures - standalone mode" {
    start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
        start_server [list overrides [list "ext-data-mode" kv "loglevel" debug]] {
            set primary [srv -1 client]
            set primary_host [srv -1 host]
            set primary_port [srv -1 port]
            set replica [srv 0 client]
            
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
            wait_for_condition 50 100 {
                [lindex [$replica role] 0] eq {replica}
            } else {
                fail "Replication not started"
            }
            
            # Load module on replica WITH 50% failure rate
            assert_equal {OK} [$replica module load $extdatamodule1 set_failure_percent=50]
            assert_equal {OK} [$replica external_data INIT db0 helloextdata1]
            
            # Despite the 50% failure rate on replica, data should eventually sync
            # The first attempt will fail, but retries should succeed
            wait_for_condition 100 100 {
                [$replica get primary_key1 ext] eq "primary_value1" &&
                [$replica get primary_key2 ext] eq "primary_value2"
            } else {
                fail "Replica not synchronized despite failures"
            }
            
            # Add more data on primary
            $primary set primary_key3 "primary_value3" EXT
            $primary set primary_key4 "primary_value4" EXT
            
            # Verify replica eventually syncs new data despite failures
            wait_for_condition 100 100 {
                [$replica get primary_key3 ext] eq "primary_value3" &&
                [$replica get primary_key4 ext] eq "primary_value4"
            } else {
                fail "Replica not synchronized with new data despite failures"
            }
            
            # Verify system stability - both primary and replica should be operational
            assert_equal "primary_value1" [$primary get primary_key1 ext]
            assert_equal "primary_value1" [$replica get primary_key1 ext]
        }
    }
}

# Test cluster mode with simulated failures
start_cluster 1 1 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
    
    test "External storage with simulated failures - cluster mode" {
        wait_for_cluster_state ok
        
        set primary [srv 0 client]
        set replica [srv -1 client]
        
        # Load module on primary without failures
        assert_equal {OK} [$primary module load $extdatamodule1]
        assert_equal {OK} [$primary external_data INIT db0 helloextdata1]
        
        # Add data on primary
        $primary select 0
        $primary set cluster_key1 "cluster_value1" EXT
        $primary set cluster_key2 "cluster_value2" EXT
        
        # Load module on replica WITH 50% failure rate
        assert_equal {OK} [$replica module load $extdatamodule1 set_failure_percent=50]
        assert_equal {OK} [$replica external_data INIT db0 helloextdata1]
        
        # Despite the 50% failure rate on replica, data should eventually sync
        wait_for_condition 100 100 {
            [$replica get cluster_key1 ext] eq "cluster_value1" &&
            [$replica get cluster_key2 ext] eq "cluster_value2"
        } else {
            fail "Cluster replica not synchronized despite failures"
        }
        
        # Add more data on primary
        $primary set cluster_key3 "cluster_value3" EXT
        $primary set cluster_key4 "cluster_value4" EXT
        
        # Verify replica eventually syncs new data despite failures
        wait_for_condition 100 100 {
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
    }
}


# Test replication with 100% failure rate and replica restart
test "External storage replication with 100% failures and restart - standalone mode" {
    start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
        start_server [list overrides [list "ext-data-mode" kv "loglevel" debug]] {
            set primary [srv -1 client]
            set primary_host [srv -1 host]
            set primary_port [srv -1 port]
            set replica [srv 0 client]
            
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
            wait_for_condition 50 100 {
                [lindex [$replica role] 0] eq {replica}
            } else {
                fail "Replication not started"
            }
            
            # Load module on replica WITH 100% failure rate (all sets fail)
            assert_equal {OK} [$replica module load $extdatamodule1 set_failure_percent=100]
            assert_equal {OK} [$replica external_data INIT db0 helloextdata1]
            
            # Wait 1 second with all failures
            after 1000
            
            # Verify data is NOT synced yet due to 100% failures
            assert_equal {} [$replica get primary_key1 ext]
            assert_equal {} [$replica get primary_key2 ext]
            
            # Restart replica to clear the failure state
            restart_server 0 true false
            set replica [srv 0 client]
            
            # Reload module on replica WITHOUT failures this time
            assert_equal {OK} [$replica module load $extdatamodule1]
            assert_equal {OK} [$replica external_data INIT db0 helloextdata1]
            
            # Now data should sync successfully
            wait_for_condition 100 100 {
                [$replica get primary_key1 ext] eq "primary_value1" &&
                [$replica get primary_key2 ext] eq "primary_value2"
            } else {
                fail "Replica not synchronized after restart"
            }
            
            # Add more data on primary to verify ongoing replication
            $primary set primary_key3 "primary_value3" EXT
            
            # Verify new data syncs
            wait_for_condition 100 100 {
                [$replica get primary_key3 ext] eq "primary_value3"
            } else {
                fail "Replica not synchronized with new data after restart"
            }
            
            # Verify system stability
            assert_equal "primary_value1" [$primary get primary_key1 ext]
            assert_equal "primary_value1" [$replica get primary_key1 ext]
        }
    }
}

# Test cluster mode with 100% failure rate and replica restart
start_cluster 1 1 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
    
    test "External storage with 100% failures and restart - cluster mode" {
        wait_for_cluster_state ok
        
        set primary [srv 0 client]
        set replica [srv -1 client]
        
        # Load module on primary without failures
        assert_equal {OK} [$primary module load $extdatamodule1]
        assert_equal {OK} [$primary external_data INIT db0 helloextdata1]
        
        # Add data on primary
        $primary select 0
        $primary set cluster_key1 "cluster_value1" EXT
        $primary set cluster_key2 "cluster_value2" EXT
        
        # Load module on replica WITH 100% failure rate (all sets fail)
        assert_equal {OK} [$replica module load $extdatamodule1 set_failure_percent=100]
        assert_equal {OK} [$replica external_data INIT db0 helloextdata1]
        
        # Wait 1 second with all failures
        after 1000
        
        # Verify data is NOT synced yet due to 100% failures
        assert_equal {} [$replica get cluster_key1 ext]
        assert_equal {} [$replica get cluster_key2 ext]
        
        # Restart replica to clear the failure state
        restart_server -1 true false
        set replica [srv -1 client]
        wait_for_cluster_state ok
        
        # Reload module on replica and use config set to disable failures
        assert_equal {OK} [$replica module load $extdatamodule1 set_failure_percent=100]
        assert_equal {OK} [$replica external_data INIT db0 helloextdata1]
        
        # Use runtime configuration to set failure rate to 0
        $replica config set helloextdata1.set_failure_percent 0
        
        # Wait for cluster to stabilize
        wait_for_cluster_state ok
        
        # Now data should sync successfully
        wait_for_condition 100 100 {
            [$replica get cluster_key1 ext] eq "cluster_value1" &&
            [$replica get cluster_key2 ext] eq "cluster_value2"
        } else {
            fail "Cluster replica not synchronized after restart"
        }
        
        # Add more data on primary to verify ongoing replication
        $primary set cluster_key3 "cluster_value3" EXT
        
        # Verify new data syncs
        wait_for_condition 100 100 {
            [$replica get cluster_key3 ext] eq "cluster_value3"
        } else {
            fail "Cluster replica not synchronized with new data after restart"
        }
        
        # Verify system stability
        assert_equal "cluster_value1" [$primary get cluster_key1 ext]
        assert_equal "cluster_value1" [$replica get cluster_key1 ext]
        
        # Verify cluster state is still ok
        wait_for_cluster_state ok
    }
}
