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
        r debug sleep 1
        
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
        r debug sleep 1

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
start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
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
start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip"]] {
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
start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "cluster"]] {
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
start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "cluster"]] {
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

# Tag: addon - can be run separately with ./runtest --tags addon
start_server [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "addon" "slow"]] {
    test {FLUSHDB and SWAPDB performance with large dataset (should complete in under 500ms)} {
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
        
        # Assert FLUSHDB completes in under 500ms
        # This will FAIL with the current O(n) implementation
        # demonstrating the need for a more efficient flush mechanism (e.g., native flush API)
        if {$flush_time >= 500} {
            fail "FLUSHDB took ${flush_time}ms, expected < 500ms (demonstrates O(n) limitation - needs native flush API)"
        }
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

# Test SWAPDB with cluster mode
start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "cluster" "singledb:skip"]] {
    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
    
    test {SWAPDB swaps external data between two databases (cluster)} {
        wait_for_cluster_state ok
        
        # Load module and initialize external storage
        assert_equal {OK} [r module load $extdatamodule1]
        assert_equal {OK} [r external_data INIT db0 helloextdata1]
        assert_equal {OK} [r external_data INIT db1 helloextdata1]
        assert_equal {OK} [r external_data INIT db2 helloextdata1]
        
        # Set keys in db0
        r select 0
        assert_equal {OK} [r set cluster_swap_key0_1 cluster_swap_value0_1 ext]
        assert_equal {OK} [r set cluster_swap_key0_2 cluster_swap_value0_2 ext]
        
        # Set keys in db1
        r select 1
        assert_equal {OK} [r set cluster_swap_key1_1 cluster_swap_value1_1 ext]
        assert_equal {OK} [r set cluster_swap_key1_2 cluster_swap_value1_2 ext]
        
        # Set keys in db2 (should remain unaffected)
        r select 2
        assert_equal {OK} [r set cluster_swap_key2_1 cluster_swap_value2_1 ext]
        
        # Verify keys exist before swap using GET, KEYS, and SCAN
        r select 0
        assert_equal cluster_swap_value0_1 [r get cluster_swap_key0_1 ext]
        assert_equal cluster_swap_value0_2 [r get cluster_swap_key0_2 ext]
        assert_equal {cluster_swap_key0_1 cluster_swap_key0_2} [lsort [r keys cluster_swap_key* ext]]
        set keys_db0 [lsort [scan_keys 0 "string" "cluster_swap_key*" "ext"]]
        assert_equal {cluster_swap_key0_1 cluster_swap_key0_2} $keys_db0
        
        r select 1
        assert_equal cluster_swap_value1_1 [r get cluster_swap_key1_1 ext]
        assert_equal cluster_swap_value1_2 [r get cluster_swap_key1_2 ext]
        assert_equal {cluster_swap_key1_1 cluster_swap_key1_2} [lsort [r keys cluster_swap_key* ext]]
        set keys_db1 [lsort [scan_keys 0 "string" "cluster_swap_key*" "ext"]]
        assert_equal {cluster_swap_key1_1 cluster_swap_key1_2} $keys_db1
        
        r select 2
        assert_equal cluster_swap_value2_1 [r get cluster_swap_key2_1 ext]
        assert_equal {cluster_swap_key2_1} [r keys cluster_swap_key* ext]
        
        # Execute SWAPDB
        assert_equal {OK} [r swapdb 0 1]
        
        # Verify external data is swapped using GET, KEYS, and SCAN
        r select 0
        assert_equal cluster_swap_value1_1 [r get cluster_swap_key1_1 ext]
        assert_equal cluster_swap_value1_2 [r get cluster_swap_key1_2 ext]
        assert_equal {} [r get cluster_swap_key0_1 ext]
        assert_equal {cluster_swap_key1_1 cluster_swap_key1_2} [lsort [r keys cluster_swap_key* ext]]
        set keys_db0_after [lsort [scan_keys 0 "string" "cluster_swap_key*" "ext"]]
        assert_equal {cluster_swap_key1_1 cluster_swap_key1_2} $keys_db0_after
        
        r select 1
        assert_equal cluster_swap_value0_1 [r get cluster_swap_key0_1 ext]
        assert_equal cluster_swap_value0_2 [r get cluster_swap_key0_2 ext]
        assert_equal {} [r get cluster_swap_key1_1 ext]
        assert_equal {cluster_swap_key0_1 cluster_swap_key0_2} [lsort [r keys cluster_swap_key* ext]]
        set keys_db1_after [lsort [scan_keys 0 "string" "cluster_swap_key*" "ext"]]
        assert_equal {cluster_swap_key0_1 cluster_swap_key0_2} $keys_db1_after
        
        # Verify db2 is unaffected
        r select 2
        assert_equal cluster_swap_value2_1 [r get cluster_swap_key2_1 ext]
        assert_equal {cluster_swap_key2_1} [r keys cluster_swap_key* ext]
    }
    
    test {SWAPDB with same database leaves external data unchanged (cluster)} {
        # Set keys in db0
        r select 0
        assert_equal {OK} [r set cluster_same_key0_1 cluster_same_value0_1 ext]
        
        # Verify key exists before swap
        assert_equal cluster_same_value0_1 [r get cluster_same_key0_1 ext]
        
        # Execute SWAPDB with same database
        assert_equal {OK} [r swapdb 0 0]
        
        # Verify external data is unchanged
        assert_equal cluster_same_value0_1 [r get cluster_same_key0_1 ext]
    }
    
    test {SWAPDB: client sees swapped external data without SELECT (cluster)} {
        # This test verifies that after SWAPDB, a client connected to a database
        # automatically sees the new external data without needing to call SELECT
        # (same behavior as with memory keys)
        
        wait_for_cluster_state ok
        
        # Initialize databases
        r select 7
        assert_equal {OK} [r external_data INIT db7 helloextdata1]
        r select 8
        assert_equal {OK} [r external_data INIT db8 helloextdata1]
        
        # Set up data in db7
        r select 7
        assert_equal {OK} [r set cluster_client_key7_1 cluster_client_value7_1 ext]
        assert_equal {OK} [r set cluster_client_key7_2 cluster_client_value7_2 ext]
        
        # Set up data in db8
        r select 8
        assert_equal {OK} [r set cluster_client_key8_1 cluster_client_value8_1 ext]
        assert_equal {OK} [r set cluster_client_key8_2 cluster_client_value8_2 ext]
        
        # Create a second client connected to db7
        set client2 [valkey_cluster_client 0 0]
        $client2 select 7
        
        # Verify client2 sees db7's data before swap
        assert_equal cluster_client_value7_1 [$client2 get cluster_client_key7_1 ext]
        assert_equal cluster_client_value7_2 [$client2 get cluster_client_key7_2 ext]
        assert_equal {cluster_client_key7_1 cluster_client_key7_2} [lsort [$client2 keys cluster_client_key* ext]]
        
        # Verify client2 does NOT see db8's data
        assert_equal {} [$client2 get cluster_client_key8_1 ext]
        assert_equal {} [$client2 get cluster_client_key8_2 ext]
        
        # Execute SWAPDB using the main client (r)
        r select 0
        assert_equal {OK} [r swapdb 7 8]
        
        # Now client2 (still logically connected to db7, but without calling SELECT again)
        # should see db8's data because db7 and db8 were swapped
        assert_equal cluster_client_value8_1 [$client2 get cluster_client_key8_1 ext]
        assert_equal cluster_client_value8_2 [$client2 get cluster_client_key8_2 ext]
        assert_equal {cluster_client_key8_1 cluster_client_key8_2} [lsort [$client2 keys cluster_client_key* ext]]
        
        # Verify client2 does NOT see db7's old data anymore
        assert_equal {} [$client2 get cluster_client_key7_1 ext]
        assert_equal {} [$client2 get cluster_client_key7_2 ext]
        
        # Verify using SCAN as well
        set keys_client2 {}
        set cur 0
        while 1 {
            set res [$client2 scan $cur type "string" match "cluster_client_key*" ext]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys_client2 {*}$k
            if {$cur == 0} break
        }
        assert_equal {cluster_client_key8_1 cluster_client_key8_2} [lsort $keys_client2]
        
        # Cleanup
        $client2 close
    }
}
