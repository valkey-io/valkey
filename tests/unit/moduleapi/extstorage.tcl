set someothermodule [file normalize tests/modules/timer.so]
set storagemodule1 [file normalize tests/modules/extstorage/extstorage1.so]
set storagemodule2 [file normalize tests/modules/extstorage/extstorage2.so]
set filtermodule1 [file normalize tests/modules/extstorage/extfilter1.so]
set filtermodule2 [file normalize tests/modules/extstorage/extfilter2.so]

start_server {tags {"external_data external:skip"}} {
    test {Running EXTERNAL_DATA LOADED with switched off external data fails} {
        assert_error {ERR External data commands are unavailable with ext-data-mode off} {r external_data loaded storage}
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
}

start_server [list overrides [list "ext-data-mode" test] tags [list "external:skip"]] {
    test {Loading storage module does affect LOADED commands} {
        # success on storage load
        assert_equal {OK} [r module load $storagemodule1]
        assert_equal [list hellostorage1] [r external_data loaded storage]
        assert_equal [list ] [r external_data loaded filter]

        # unload non-loaded module
        set code [catch {r module unload $storagemodule2}]
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

        # unload loaded modules
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

        # unload non-loaded module
        set code [catch {r module unload $filtermodule2}]
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

        # unload loaded modules
        assert_equal {OK} [r module unload hellofilter2]
        assert_equal [list hellofilter1] [r external_data loaded filter]
        assert_equal {OK} [r module unload hellofilter1]
        assert_equal [list ] [r external_data loaded filter]
        assert_equal [list ] [r external_data loaded storage]
    }
}

start_server [list overrides [list "ext-data-mode" test] tags [list "external:skip"]] {
    test {Initializing storage module does affect STATS commands} {
        # STATS ok
        assert_equal [list ] [r external_data stats storage]
    }
}

start_server [list overrides [list "ext-data-mode" test] tags [list "external:skip"]] {
    test {Initializing filter module does affect STATS commands} {
        # STATS ok
        assert_equal [list ] [r external_data loaded filter]
    }
}
