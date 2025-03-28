set someothermodule [file normalize tests/modules/timer.so]
set storagemodule1 [file normalize tests/modules/extstorage/extstorage1.so]
set storagemodule2 [file normalize tests/modules/extstorage/extstorage2.so]
set filtermodule1 [file normalize tests/modules/extstorage/extfilter1.so]
set filtermodule2 [file normalize tests/modules/extstorage/extfilter2.so]
set ext_data_off_err "ERR External data commands are unavailable with ext-data-mode off"

start_server {tags {"external_data external:skip"}} {
    test {Running EXTERNAL_DATA commands with switched off external data fails} {
        assert_error $ext_data_off_err {r external_data init db0 storage storagemodule1 filter filtermodule1}
        assert_error $ext_data_off_err {r external_data loaded storage}
        assert_error $ext_data_off_err {r external_data stats storage}
    }
}

start_server [list overrides [list "ext-data-mode" test] tags [list "external:skip"]] {
    test {Running EXTERNAL_DATA LOADED with switched on external data succeeds} {
        assert_equal [list ] [r external_data loaded storage]
        assert_equal [list ] [r external_data loaded filter]
        assert_error {ERR Unknown module type (storage or filter expected)} {r external_data loaded anything}
    }

    test {Loading some module does not affect LOADED commands} {
        assert_equal {OK} [r module load $someothermodule]
        assert_equal [list ] [r external_data loaded storage]
        assert_equal [list ] [r external_data loaded filter]
    }

    test {Dropping unloaded module fails} {
        assert_error {ERR db0 is not initialized} {r external_data drop db0}
    }
}

start_server [list overrides [list "ext-data-mode" test] tags [list "external:skip"]] {
    test {Loading storage module does affect LOADED commands} {
        # success on storage load
        assert_equal {OK} [r module load $storagemodule1]
        assert_equal [list hellostorage1] [r external_data loaded storage]
        assert_equal [list ] [r external_data loaded filter]

        # unload non-loaded module fails
        set code [catch {r module unload hellostorage2}]
        if {$code == 0} {
            puts "expected error on unloading non-loaded module, got none"
            exit 1
        }

        # same storage loaded twice
        set code [catch {r module load $storagemodule1}]
        if {$code == 0} {
            puts "expected error on loading same module twice, got none"
            exit 1
        }

        # success on several storages load
        assert_equal {OK} [r module load $storagemodule2] 
        assert_equal [list hellostorage1 hellostorage2] [r external_data loaded storage]
        assert_equal [list ] [r external_data loaded filter]

        # unload loaded modules ok
        assert_equal {OK} [r module unload hellostorage1]
        assert_equal [list hellostorage2] [r external_data loaded storage]
        assert_equal {OK} [r module unload hellostorage2]
        assert_equal [list ] [r external_data loaded storage]
        assert_equal [list ] [r external_data loaded filter]
    }
}

start_server [list overrides [list "ext-data-mode" test] tags [list "external:skip"]] {
    test {Loading filter module does affect LOADED commands} {
        # success on filter load
        assert_equal {OK} [r module load $filtermodule1]
        assert_equal [list hellofilter1] [r external_data loaded filter]
        assert_equal [list ] [r external_data loaded storage]

        # unload non-loaded module fails
        set code [catch {r module unload hellofilter2}]
        if {$code == 0} {
            puts "expected error on unloading non-loaded module, got none"
            exit 1
        }

        # same filter loaded twice
        set code [catch {r module load $filtermodule1}]
        if {$code == 0} {
            puts "expected error on loading same module twice, got none"
            exit 1
        }

        # success on several filters load
        assert_equal {OK} [r module load $filtermodule2]
        assert_equal [list hellofilter1 hellofilter2] [r external_data loaded filter]
        assert_equal [list ] [r external_data loaded storage]

        # unload loaded modules ok
        assert_equal {OK} [r module unload hellofilter2]
        assert_equal [list hellofilter1] [r external_data loaded filter]
        assert_equal {OK} [r module unload hellofilter1]
        assert_equal [list ] [r external_data loaded filter]
        assert_equal [list ] [r external_data loaded storage]
    }
}

start_server [list overrides [list "ext-data-mode" test] tags [list "external:skip"]] {
    test {Initializing and dropping db affects STATS commands} {
        # STATS ok with modules non-loaded
        assert_equal [list ] [r external_data stats filter]
        assert_equal [list ] [r external_data stats storage]

        # check init input arguments
        assert_error {ERR failed to parse db number from db, expect db0, db10, etc.} {r external_data INIT db STORAGE hellostorage1 FILTER hellofilter1}
        assert_error {ERR db number 16 exceeds used on server 0-15} {r external_data INIT db16 STORAGE hellostorage1 FILTER hellofilter1}
        assert_error {ERR wrong number of arguments for 'external_data|init' command} {r external_data INIT db0 STORAGE  FILTER hellofilter1}
        assert_error {ERR wrong number of arguments for 'external_data|init' command} {r external_data INIT db0 STORAGE hellostorage1 FILTER }

        # you need to load modules to init
        assert_error {ERR storage module hellostorage1 is not loaded} {r external_data INIT db0 STORAGE hellostorage1 FILTER hellofilter1}
        assert_equal {OK} [r module load $storagemodule1]
        assert_error {ERR filter module hellofilter3 is not loaded} {r external_data INIT db0 FILTER hellofilter3 STORAGE hellostorage1}
        assert_equal {OK} [r module load $filtermodule1]

        # STATS ok with modules loaded
        assert_equal [list ] [r external_data stats filter]
        assert_equal [list ] [r external_data stats storage]
        assert_equal {OK} [r external_data INIT db0 STORAGE hellostorage1 FILTER hellofilter1]

        assert_equal [list db0:hellofilter1] [r external_data stats filter]
        assert_equal [list db0:hellostorage1] [r external_data stats storage]
        assert_equal {OK} [r module load $storagemodule2]
        assert_equal {OK} [r module load $filtermodule2]
        assert_equal {OK} [r external_data INIT db1 STORAGE hellostorage2 FILTER hellofilter2]
        assert_equal [list db0:hellofilter1 db1:hellofilter2] [r external_data stats filter]
        assert_equal [list db0:hellostorage1 db1:hellostorage2] [r external_data stats storage]

        # you can't init the same db without dropping its currently used modules
        assert_error {ERR db0 is already initialized} {r external_data INIT db0 STORAGE hellostorage1 FILTER hellofilter1}

        # unload loaded and inited module fails
        assert_error {ERR Error unloading module: operation not possible.} {r module unload hellostorage1}
        assert_error {ERR Error unloading module: operation not possible.} {r module unload hellofilter1}

        # dropping succeeds
        assert_error {ERR Leads to persistent storage data loss for db0, use FORCE if sure} {r external_data drop db0}
        assert_equal {OK} [r external_data drop db0 FORCE]
        assert_equal [list db1:hellofilter2] [r external_data stats filter]
        assert_equal [list db1:hellostorage2] [r external_data stats storage]
        assert_equal {OK} [r module unload hellostorage1]
        assert_equal {OK} [r module unload hellofilter1]

        # init again succeeds
        assert_equal {OK} [r external_data INIT db0 STORAGE hellostorage2 FILTER hellofilter2]
        assert_equal [list db0:hellofilter2 db1:hellofilter2] [r external_data stats filter]
        assert_equal [list db0:hellostorage2 db1:hellostorage2] [r external_data stats storage]

        # cleanup ok
        assert_equal {OK} [r external_data drop db0 FORCE]
        assert_equal {OK} [r external_data drop db1 FORCE]
        assert_equal [list ] [r external_data stats filter]
        assert_equal [list ] [r external_data stats storage]
        assert_equal {OK} [r module unload hellostorage2]
        assert_equal {OK} [r module unload hellofilter2]
    }
}

start_server [list overrides [list "ext-data-mode" test "loglevel" debug] tags [list "external:skip"]] {
    test {Reading keys from storage works} {
        # init
        assert_equal {OK} [r module load $storagemodule1]
        assert_equal {OK} [r module load $filtermodule1]
        assert_error {ERR db0 is not initialized} {r external_data debug db0 filter set k v}
        assert_error {ERR db0 is not initialized} {r external_data debug db0 storage set k v}
        assert_equal {OK} [r external_data INIT db0 STORAGE hellostorage1 FILTER hellofilter1]
        assert_error {ERR unknown subcommand somecommand} {r external_data debug db0 somecommand set k v}
        assert_equal {OK} [r external_data INIT db1 STORAGE hellostorage1 FILTER hellofilter1]

        # filter RO, storage RO = nil
        assert_equal {OK} [r external_data debug db0 storage setro]
        assert_equal {OK} [r external_data debug db0 filter setro]
        assert_error {ERR k set failed} {r external_data debug db0 storage set k v}
        assert_error {ERR k set failed} {r external_data debug db0 filter set k v}
        assert_equal {} [r get k]
        assert_equal {OK} [r select 1]
        assert_equal {} [r get k]
        assert_equal {OK} [r select 0]

        # filter RO, storage OK = nil
        assert_equal {OK} [r external_data debug db0 storage dropro]
        assert_equal {OK} [r external_data debug db0 storage set k v]
        assert_error {ERR k set failed} {r external_data debug db0 filter set k v}
        assert_equal {} [r get k]
        assert_equal {OK} [r select 1]
        assert_equal {} [r get k]
        assert_equal {OK} [r select 0]

        # filter OK, storage RO = nil
        assert_equal v [r external_data debug db0 storage del k]
        assert_equal {} [r external_data debug db0 storage del k]
        assert_equal {OK} [r external_data debug db0 storage setro]
        assert_equal {OK} [r external_data debug db0 filter dropro]
        assert_error {ERR k set failed} {r external_data debug db0 storage set k v}
        assert_equal {OK} [r external_data debug db0 filter set k v]
        assert_equal {} [r get k]
        assert_equal {OK} [r select 1]
        assert_equal {} [r get k]
        assert_equal {OK} [r select 0]

        # filter ok, storage ok = OK
        assert_equal {OK} [r external_data debug db0 storage dropro]
        assert_equal {OK} [r external_data debug db0 storage set k v]
        assert_equal {OK} [r external_data debug db0 storage set k v]
        assert_equal {OK} [r external_data debug db0 filter set k v]
        assert_equal v [r get k]
        assert_equal {OK} [r select 1]
        assert_equal {} [r get k]
        assert_equal {OK} [r select 0]

        # filter not, storage ok = nil
        assert_equal 1 [r external_data debug db0 filter del k]
        assert_equal 0 [r external_data debug db0 filter del k]
        assert_equal {} [r get k]
        assert_equal {OK} [r select 1]
        assert_equal {} [r get k]
        assert_equal {OK} [r select 0]

        # filter OK, storage not = nil
        assert_equal {OK} [r external_data debug db0 filter set k v]
        assert_equal v [r external_data debug db0 storage del k]
        assert_equal {} [r get k]
        assert_equal {OK} [r select 1]
        assert_equal {} [r get k]
        assert_equal {OK} [r select 0]

        # filter not, storage not = nil
        assert_equal 1 [r external_data debug db0 filter del k]
        assert_equal {} [r get k]
        assert_equal {OK} [r select 1]
        assert_equal {} [r get k]
        assert_equal {OK} [r select 0]

        # ToDo: add sharded cluster test for key read (including several nodes with MOVE scenario)
        # ToDo: remove v from filter add (storing only k)
    }
}
