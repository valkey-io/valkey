set testmodule [file normalize tests/modules/path_hash_notification.so]

tags {"modules pathhash external:skip"} {
    start_server [list overrides [list loadmodule $testmodule]] {
        test {Path Hash notification callback can delete or replace its key} {
            foreach mode {del replace} {
                assert_equal OK [r pathhashnotification.mode $mode]
                foreach {name paths command expected} {
                    last-field {{p f one g two}} {phdel uaf:key p f g} 2
                    last-path {{p f one}} {phdel uaf:key p} 1
                    partial-field {{p f one g two}} {phdel uaf:key p f} 1
                    nonfinal-path {{p f one} {q f two}} {phdel uaf:key p} 1
                    last-prefix {{p f one}} {phdelprefix uaf:key p} 1
                    partial-prefix {{p f one} {q f two}} {phdelprefix uaf:key p} 1
                } {
                    r del uaf:key
                    foreach path $paths {
                        r phset uaf:key [lindex $path 0] fields 1 [lindex $path 1] [lindex $path 2]
                        if {[llength $path] == 5} {
                            r phset uaf:key [lindex $path 0] fields 1 [lindex $path 3] [lindex $path 4]
                        }
                    }
                    assert_equal $expected [r {*}$command]
                    if {$mode eq "replace"} {
                        assert_equal string [r type uaf:key]
                        assert_equal replacement [r get uaf:key]
                    } else {
                        assert_equal 0 [r exists uaf:key]
                    }
                }
            }
        }

        test {Path Hash notification callback leaves normal final-key cleanup intact} {
            r pathhashnotification.mode none
            r del uaf:key
            r phset uaf:key p fields 1 f one
            assert_equal 1 [r phdel uaf:key p]
            assert_equal 0 [r exists uaf:key]
            r phset uaf:key p fields 1 f one
            assert_equal 1 [r phdelprefix uaf:key p]
            assert_equal 0 [r exists uaf:key]
        }
    }
}
