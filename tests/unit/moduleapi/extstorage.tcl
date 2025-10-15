set someothermodule [file normalize tests/modules/timer.so]
set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]
set extdatamodule2 [file normalize tests/modules/extstorage/extdata2.so]
set ext_data_off_err "ERR External data commands are unavailable with ext-data-mode off"

start_server {tags {"external_data external:skip"}} {
    test {Running EXTERNAL_DATA commands with switched off external data fails} {
        assert_error $ext_data_off_err {r external_data init db0 helloextdata1}
        assert_error $ext_data_off_err {r external_data loaded}
        assert_error $ext_data_off_err {r external_data stats}
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
        assert_equal {OK} [r external_data debug db0 storage setro]
        assert_equal {OK} [r external_data debug db0 filter setro]
        assert_error {ERR k set failed} {r external_data debug db0 storage set k v}
        assert_error {ERR k set failed} {r external_data debug db0 filter set k}
        assert_equal {OK} [r select 0]
        assert_equal {} [r get k ext]
        assert_equal {OK} [r select 1]
        assert_equal {} [r get k ext]
        assert_equal {OK} [r select 0]

        # filter RO, storage OK = nil
        assert_equal {OK} [r external_data debug db0 storage dropro]
        assert_equal {OK} [r external_data debug db0 storage set k v]
        assert_error {ERR k set failed} {r external_data debug db0 filter set k}
        assert_equal {} [r get k ext]
        assert_equal {OK} [r select 1]
        assert_equal {} [r get k ext]
        assert_equal {OK} [r select 0]

        # filter OK, storage RO = nil
        assert_equal v [r external_data debug db0 storage del k]
        assert_equal {} [r external_data debug db0 storage del k]
        assert_equal {OK} [r external_data debug db0 storage setro]
        assert_equal {OK} [r external_data debug db0 filter dropro]
        assert_error {ERR k set failed} {r external_data debug db0 storage set k v}
        assert_equal {OK} [r external_data debug db0 filter set k]
        assert_equal {} [r get k ext]
        assert_equal {OK} [r select 1]
        assert_equal {} [r get k ext]
        assert_equal {OK} [r select 0]

        # filter ok, storage ok = OK
        assert_equal {OK} [r external_data debug db0 storage dropro]
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

### Sharded
start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "cluster"]] {
    # we assume that all cross-requests to and from another nodes are made in a usual Valkey way
    # that's why there are no tests with MOVED during set/get here
    test "Cluster should start ok" {
        wait_for_cluster_state ok
    }

    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

    test "External storage works with single sharded" {
        # init
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
}

start_cluster 1 0 [list overrides [list "ext-data-mode" kv "loglevel" debug] tags [list "external:skip" "cluster"]] {
    # we assume that all cross-requests to and from another nodes are made in a usual Valkey way
    # that's why there are no tests with MOVED during set/get here
    test "Cluster should start ok" {
        wait_for_cluster_state ok
    }

    set extdatamodule1 [file normalize tests/modules/extstorage/extdata1.so]

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
