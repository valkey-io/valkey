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

start_server [list overrides [list "ext-data-mode" kv "loglevel" debug "ext-data-timeout" 0] tags [list "external:skip"]] {
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

start_server [list overrides [list "ext-data-mode" kv "loglevel" debug "ext-data-timeout" 0] tags [list "external:skip"]] {
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

start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
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

start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
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

    test "ext-data-store-by-size not set: large key/value stored in memory" {
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
start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "cluster"]] {
    # we assume that all cross-requests to and from another nodes are made in a usual Valkey way
    # that's why there are no tests with MOVED during set/get here
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    test "External storage works with single sharded" {
        # init
        wait_for_cluster_state ok
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]

        # filter ok, storage ok = OK
        assert_equal {OK} [r external_data debug db0 storage set k v]
        assert_equal {OK} [r external_data debug db0 storage set k v]
        assert_equal {OK} [r external_data debug db0 filter set k]
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

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "cluster"]] {
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

start_cluster 1 0 [list overrides [list "loglevel" debug] tags [list "external:skip" "cluster"]] {
    set ext_data_off_err "ERR External data commands are unavailable with ext-data-mode off"
    test "SET with ext option fails when ext-data-mode is off (sharded)" {
        wait_for_cluster_state ok
        assert_error $ext_data_off_err {r set k v ext}
    }
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "cluster"]] {
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

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "cluster"]] {
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
start_server {
    overrides {
        "ext-data-mode" "kv"
        "ext-data-store-by-size" "0"
        "loglevel" "debug"
    }
} {
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

    test "DEL with EXT - Non-sharded - Different databases" {
        r select 0
        # Initialize external data for db1
        r external_data INIT db1 helloextdata1
        
        # Set keys in different databases
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
start_server {
    overrides {
        "ext-data-mode" "kv"
        "ext-data-store-by-size" "0"
    }
} {
    # Load module once for all tests in this block
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

    test "DEL with EXT - Sharded - Multiple keys with external storage" {
        r select 0
        # Set multiple keys to external storage
        r set multi_key1 "value1" ext
        r set multi_key2 "value2" ext
        r set multi_key3 "value3" ext
        
        # Verify they exist in external storage
        assert_equal "value1" [r get multi_key1 ext]
        assert_equal "value2" [r get multi_key2 ext]
        assert_equal "value3" [r get multi_key3 ext]
        
        # Delete multiple keys with EXT flag
        assert_equal 2 [r del multi_key1 multi_key2 ext]
        
        # Verify deletion
        assert_equal "" [r get multi_key1]
        assert_equal "" [r get multi_key1 ext]
        assert_equal "" [r get multi_key2]
        assert_equal "" [r get multi_key2 ext]
        assert_equal "" [r get multi_key3]
        assert_equal "value3" [r get multi_key3 ext]
    }

    test "DEL with EXT - Sharded - Different databases" {
        r select 0
        # Initialize external data for db1
        r external_data INIT db1 helloextdata1
        
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
