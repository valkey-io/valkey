start_server {tags {pathhash}} {
    test {PHSET creates a native path hash object and exact reads are binary safe} {
        set path [binary format H* 0001ff]
        set field [binary format H* 660069656c64]
        set value [binary format H* 7600616c7565ff]
        assert_equal OK [r phset tree $path fields 1 $field $value]
        assert_equal pathhash [r type tree]
        assert_equal pathhash [r object encoding tree]
        assert_equal 1 [r phpathcount tree]
        assert_equal [list $value] [r phget tree $path $field]
        assert_equal [list {}] [r phget tree $path missing]
        assert_equal [list $value {}] [r phget tree $path $field missing]
        assert_equal 0 [r phpathcount missing]
    }

    test {PHPATHCOUNT counts byte prefixes including exact and binary paths} {
        r del count-tree
        foreach path {{} a ab abc abcd abx b} {
            r phset count-tree $path fields 1 f v
        }
        assert_equal 7 [r phpathcount count-tree]
        assert_equal 7 [r phpathcount count-tree prefix {}]
        assert_equal 5 [r phpathcount count-tree prefix a]
        assert_equal 4 [r phpathcount count-tree prefix ab]
        assert_equal 1 [r phpathcount count-tree prefix abcd]
        assert_equal 0 [r phpathcount count-tree prefix z]
        assert_equal 0 [r phpathcount missing prefix a]
        set prefix [binary format H* 00ff]
        r phset count-tree $prefix fields 1 f v
        r phset count-tree "${prefix}child" fields 1 f v
        assert_equal 2 [r phpathcount count-tree prefix $prefix]
        r phdel count-tree $prefix
        assert_equal 1 [r phpathcount count-tree prefix $prefix]
        foreach key {count-tree missing} {
            assert_error ERR*syntax* {r phpathcount $key prefix}
            assert_error ERR*syntax* {r phpathcount $key match a}
            assert_error ERR*syntax* {r phpathcount $key prefix a prefix b}
        }
        r set count-wrongtype value
        assert_error WRONGTYPE* {r phpathcount count-wrongtype prefix a}
        assert_equal {{}} [r command info phcard]
    }

    test {PHSET FNX and FXX atomically apply to the complete field group} {
        r del tree
        assert_equal {} [r phset tree path fxx fields 2 f v second two]
        assert_equal 0 [r exists tree]
        assert_equal OK [r phset tree path fnx fields 2 f v second two]
        assert_equal {} [r phset tree path fnx fields 2 f replacement missing added]
        assert_equal [list v] [r phget tree path f]
        assert_equal [list {}] [r phget tree path missing]
        assert_equal OK [r phset tree path fnx fields 1 third three]
        assert_equal [list three] [r phget tree path third]
        assert_equal {} [r phset tree path fxx fields 2 f replacement missing added]
        assert_equal [list v] [r phget tree path f]
        assert_equal [list {}] [r phget tree path missing]
        assert_equal OK [r phset tree path fxx fields 2 f replacement second updated]
        assert_equal [list replacement] [r phget tree path f]
        assert_equal [list updated] [r phget tree path second]
        assert_equal OK [r phset tree path fields 2 duplicate first duplicate last]
        assert_equal [list last] [r phget tree path duplicate]
        assert_equal 1 [r phpathcount tree]
    }

    test {Exact path operations do not confuse ancestors and descendants} {
        r del tree
        r phmset tree a fields 1 f one ab fields 1 f two abc fields 1 f three
        assert_equal [list one] [r phget tree a f]
        assert_equal [list two] [r phget tree ab f]
        assert_equal [list three] [r phget tree abc f]
        assert_equal [list {}] [r phget tree abcd f]
        assert_equal 3 [r phpathcount tree]
    }

    test {PHGET, PHMGET, and PHGETALL preserve field semantics} {
        r del tree
        r phset tree path fields 2 f1 v1 f2 v2
        r phmset tree other fields 2 f1 ignored f1 other-v1 third fields 1 f3 v3
        set values [dict create {*}[r phgetall tree path]]
        assert_equal v1 [dict get $values f1]
        assert_equal v2 [dict get $values f2]
        assert_equal {v2 {} v1} [r phget tree path f2 missing f1]
        assert_equal {v1 v2 other-v1 v3 {} v1} \
            [r phmget tree path fields 2 f1 f2 other fields 1 f1 \
                third fields 1 f3 missing fields 1 f path fields 1 f1]
        assert_equal {v2 {} v1} [r phmget tree path fields 3 f2 missing f1]
        assert_equal {{} {}} [r phget missing path f1 f2]
        assert_equal {} [r phgetall tree missing]
    }

    test {PHPATHEXISTS checks exact logical paths only} {
        r del tree
        r phmset tree ab fields 1 f one abcd fields 1 f two
        assert_equal 0 [r phpathexists tree a]
        assert_equal 1 [r phpathexists tree ab]
        assert_equal 0 [r phpathexists tree abc]
        assert_equal 1 [r phpathexists tree abcd]
        assert_equal 0 [r phpathexists missing ab]
    }

    test {PHPATHEXISTS supports root and binary paths and replaces PHEXISTS} {
        r del tree
        set path [binary format H* 0061ff]
        r phset tree {} fields 1 f root
        r phset tree $path fields 1 f binary
        assert_equal 1 [r phpathexists tree {}]
        assert_equal 1 [r phpathexists tree $path]
        assert_equal 0 [r phpathexists tree [binary format H* 0061]]
        assert_equal {{}} [r command info phexists]
        assert_error {*unknown command*} {r phexists tree $path}
    }

    test {PHPREFIXES COUNT 1 replaces longest-prefix lookup} {
        r del tree
        r phset tree {} fields 1 root root-value
        r phset tree a fields 1 f v-a
        r phset tree abc fields 2 f1 v1 f2 v2
        r phset tree abcdef fields 1 f deep
        assert_equal {abc} [r phprefixes tree abczzz count 1]
        assert_equal {3} [r phprefixes tree abczzz lengths count 1]
        assert_equal [list [list abc [list v1 {} v2]]] \
            [r phprefixes tree abczzz fields 3 f1 missing f2 count 1]
        set match [lindex [r phprefixes tree abczzz withvalues count 1] 0]
        assert_equal abc [lindex $match 0]
        assert_equal {f1 v1 f2 v2} [lsort -stride 2 [lindex $match 1]]
        assert_equal [list {}] [r phprefixes tree zzz count 1]
        assert_equal {0} [r phprefixes tree zzz lengths count 1]
        assert_equal {} [r phprefixes missing anything count 1]
        r del tree
        r phset tree abc fields 1 f v
        assert_equal {} [r phprefixes tree zzz count 1]
        assert_equal {{}} [r command info phlongest]
    }

    test {PHPREFIXES EXCLUDESELF paginates strict ancestors and handles root and limits} {
        r del tree
        foreach path {{} a ab abc abcd} {r phset tree $path fields 1 f $path}
        assert_equal {abcd abc} [r phprefixes tree abcdef count 2]
        assert_equal {ab a} [r phprefixes tree abc excludeself count 2]
        assert_equal [list {}] [r phprefixes tree a excludeself count 2]
        assert_equal {} [r phprefixes tree {} excludeself]
        assert_equal {2 1 0} [r phprefixes tree abc lengths excludeself maxpathlen 2]
        assert_equal {2 1 0} [r phprefixes tree abc lengths excludeself maxpathlen 3]
        assert_equal {3 2} [r phprefixes tree abcdef lengths excludeself maxpathlen 3 count 2]
        # The boundary need not be stored, including after concurrent deletion.
        r phdel tree abc
        assert_equal {ab a} [r phprefixes tree abc excludeself count 2]
        assert_error ERR*syntax* {r phprefixes tree abc excludeself excludeself}
        assert_error ERR*syntax* {r phprefixes missing abc excludeself excludeself}
        assert_error ERR*syntax* {r phprefixes tree abc exclusive}
        set path [binary format H* 00ff]
        r phset tree $path fields 1 f binary
        r phset tree "${path}x" fields 1 f child
        assert_equal {2 0} [r phprefixes tree "${path}x" lengths excludeself]
        assert_equal [list [list 2 [list binary {} binary]]] \
            [r phprefixes tree "${path}x" lengths fields 3 f missing f count 1 excludeself]
    }

    test {PHPREFIXES COUNT pagination matches the full ancestor list} {
        r del tree
        foreach path {{} a ab abc abcd abcde abcdef} {r phset tree $path fields 1 f $path}
        foreach query {abcdef abcdefgh z {}} {
            set full [r phprefixes tree $query]
            set boundary $query
            set options {}
            set paths {}
            set pages 0
            while 1 {
                set page [r phprefixes tree $boundary count 2 {*}$options]
                if {[llength $page] == 0} break
                assert {[llength $page] <= 2}
                lappend paths {*}$page
                set boundary [lindex $page end]
                set options {excludeself}
                assert {[incr pages] < 10}
            }
            assert_equal $full $paths
        }
    }

    test {PHPREFIXES orders ancestors and applies MAXPATHLEN before deepest COUNT} {
        r del tree
        foreach path {{} a ab abc abcd} {
            r phset tree $path fields 1 f "value:$path"
        }
        assert_equal {4 3 2 1 0} [r phprefixes tree abcde lengths]
        assert_equal {3 2} [r phprefixes tree abcde lengths count 2 maxpathlen 3]
        assert_equal [list [list 3 [list value:abc]] [list 2 [list value:ab]]] \
            [r phprefixes tree abcde lengths fields 1 f count 2 maxpathlen 3]
        assert_equal [list {}] [r phprefixes tree zzz maxpathlen 0]
        assert_equal [list {}] [r phprefixes tree zzz]
    }

    test {PHPREFIXES descending COUNT handles wrapped and growing match buffers} {
        r del buffer-tree
        set expected {}
        for {set i 0} {$i < 40} {incr i} {
            set path [string repeat a $i]
            r phset buffer-tree $path fields 1 f $i
            set expected [linsert $expected 0 $i]
        }
        set query [string repeat a 40]
        foreach count {1 2 7 8 9 16 32 40 41} {
            assert_equal [lrange $expected 0 [expr {$count - 1}]] \
                [r phprefixes buffer-tree $query lengths count $count]
        }
        assert_equal {20 19 18} [r phprefixes buffer-tree $query lengths count 3 maxpathlen 20]
    }

    test {PHPREFIXES validates COUNT and MAXPATHLEN against the server long range} {
        r del tree
        r del missing
        foreach path {{} a ab} {
            r phset tree $path fields 1 f v
        }
        set long_max [expr {(1 << ([s arch_bits] - 1)) - 1}]
        foreach option {count maxpathlen} {
            assert_equal {2 1 0} [r phprefixes tree abc lengths $option $long_max]
            foreach value [list -1 [expr {$long_max + 1}] 9223372036854775808] {
                foreach key {tree missing} {
                    assert_error ERR*range* {r phprefixes $key abc $option $value}
                }
            }
            foreach value {1.5 invalid} {
                assert_error ERR*integer* {r phprefixes tree abc $option $value}
            }
            # These values must not wrap to zero or a smaller limit on 32-bit builds.
            foreach value {2147483648 4294967295 4294967296 4294967297} {
                if {$value > $long_max} {
                    assert_error ERR*range* {r phprefixes tree abc $option $value}
                } else {
                    assert_equal {2 1 0} [r phprefixes tree abc lengths $option $value]
                }
            }
            assert_error ERR*syntax* {r phprefixes tree abc $option 1 $option 2}
            assert_error ERR*syntax* {r phprefixes tree abc $option}
        }
        assert_error ERR*range* {r phprefixes tree abc count 0}
        assert_error ERR*syntax* {r phprefixes tree abc maxlen 1}
        assert_equal {2} [r phprefixes tree abc lengths count 1]
        assert_equal {0} [r phprefixes tree abc lengths maxpathlen 0]
        assert_equal {2 1 0} [r phprefixes tree abc lengths]
        assert_equal PONG [r ping]
    }

    test {Prefix matching and subtree deletion are binary safe} {
        r del tree
        set p0 [binary format H* 00]
        set p1 [binary format H* 0061]
        set p2 [binary format H* 006100ff]
        set sibling [binary format H* 0062]
        set query [binary format H* 006100ff7a]
        foreach path [list {} $p0 $p1 $p2 $sibling] {
            r phset tree $path fields 1 f "value:$path"
        }
        assert_equal {4 2 1 0} [r phprefixes tree $query lengths]
        assert_equal {4} [r phprefixes tree $query lengths count 1]
        assert_equal 2 [r phpathdel tree $p1 withsubpaths]
        assert_equal [list {} $p0 $sibling] [lindex [r phpathscan tree 0 count 100] 1]
    }

    test {PHPATHDEL exact mode preserves binary descendants and sibling paths} {
        r del del-tree
        set path [binary format H* 0061ff]
        set child "${path}\x00"
        set sibling [binary format H* 0062ff]
        foreach p [list {} $path $child $sibling] {r phset del-tree $p fields 2 f one g two}
        assert_equal 1 [r phpathdel del-tree $path]
        assert_equal 0 [r phpathdel del-tree $path]
        assert_equal 3 [r phpathcount del-tree]
        foreach p [list {} $child $sibling] {assert_equal {one two} [r phget del-tree $p f g]}
        assert_equal 1 [r phpathdel del-tree {}]
        assert_equal 2 [r phpathcount del-tree]
        assert_equal 1 [r phpathdel del-tree $child]
        assert_equal {one two} [r phget del-tree $sibling f g]
    }

    test {PHDEL removes fields, prunes empty payloads, and preserves descendants} {
        r del tree
        r phset tree a fields 2 f1 v1 f2 v2
        r phset tree ab fields 1 f child
        assert_equal 1 [r phdel tree a missing f1 missing]
        assert_equal {{} v2} [r phget tree a f1 f2]
        assert_equal 1 [r phdel tree a f2]
        assert_equal 1 [r phpathcount tree]
        assert_equal [list child] [r phget tree ab f]
        assert_equal 1 [r phdel tree ab]
        assert_equal 0 [r exists tree]
        assert_equal none [r type tree]
        assert_equal 0 [r phpathcount tree]
        assert_equal 0 [r phdel tree ab]
    }

    test {PHPATHDEL deletes exact paths or byte-prefix subtrees and counts paths} {
        r del del-tree
        r phmset del-tree {} fields 1 f root ab fields 2 f one g two \
            abc fields 1 f child abx fields 1 f other
        assert_equal 1 [r phpathdel del-tree ab]
        assert_equal 3 [r phpathcount del-tree]
        assert_equal {child} [r phget del-tree abc f]
        assert_equal 0 [r phpathdel del-tree ab]
        assert_equal 2 [r phpathdel del-tree ab withsubpaths]
        assert_equal 1 [r phpathcount del-tree]
        assert_equal 1 [r phpathdel del-tree {}]
        assert_equal 0 [r phpathdel missing path withsubpaths]
        r phset del-tree abc fields 1 f child
        assert_equal 0 [r phpathdel del-tree a]
        assert_equal 1 [r phpathdel del-tree a withsubpaths]
        foreach key {del-tree missing} {
            assert_error ERR*syntax* {r phpathdel $key a invalid}
            assert_error ERR*syntax* {r phpathdel $key a withsubpaths extra}
        }
        assert_equal {{}} [r command info phdelprefix]
    }

    test {PHPATHDEL deletes only descendants and empty prefix clears the tree} {
        r del tree
        foreach path {a ab abc ac b ba} {r phset tree $path fields 1 f $path}
        assert_equal 2 [r phpathdel tree ab withsubpaths]
        set result [r phpathscan tree 0 count 100]
        assert_equal 0 [lindex $result 0]
        assert_equal {a ac b ba} [lindex $result 1]
        assert_equal 4 [r phpathdel tree {} withsubpaths]
        assert_equal 0 [r exists tree]
        assert_equal none [r type tree]
        assert_equal 0 [r phpathcount tree]
        assert_equal 0 [r phpathdel tree anything withsubpaths]
    }

    test {PHPATHDEL deletes matching paths in fixed-size chunks} {
        r del tree
        set assignments {}
        for {set i 0} {$i < 600} {incr i} {
            lappend assignments "delete:$i" fields 1 f "value:$i"
        }
        lappend assignments keep:a fields 1 f one keep:b fields 1 f two keep:c fields 1 f three
        assert_equal OK [r phmset tree {*}$assignments]
        assert_equal 603 [r phpathcount tree]
        assert_equal 600 [r phpathdel tree delete: withsubpaths]
        assert_equal 3 [r phpathcount tree]
        assert_equal {keep:a keep:b keep:c} [lindex [r phpathscan tree 0 count 100] 1]
        assert_equal 3 [r phpathdel tree keep: withsubpaths]
        assert_equal 0 [r exists tree]
    }

    test {PHPATHDEL does not copy a long shared prefix once per path} {
        if {[string match {*jemalloc*} [s mem_allocator]]} {
            r del tree
            # The index stores the shared prefix once, so the key is small.
            set prefix [string repeat x 131072]
            set assignments {}
            for {set i 0} {$i < 256} {incr i} {
                lappend assignments $prefix[binary format c $i] fields 1 f v
            }
            assert_equal OK [r phmset tree {*}$assignments]
            r debug mallctl-str thread.peak.reset VOID
            set before [r debug mallctl thread.peak.read]
            assert_equal 256 [r phpathdel tree x withsubpaths]
            set after [r debug mallctl thread.peak.read]
            # Copying every path would raise the peak by 32 MB.
            assert_lessthan [expr {$after - $before}] 4000000
        }
    } {} {needs:debug}

    test {Path Hash write commands account server dirty by logical mutations} {
        r del dirty-tree
        r del empty-dirty-tree
        r save

        assert_equal OK [r phset dirty-tree p fields 2 f1 v1 f2 v2]
        assert_equal 2 [s rdb_changes_since_last_save]
        assert_equal {} [r phset dirty-tree p fnx fields 2 f1 ignored missing ignored]
        assert_equal 2 [s rdb_changes_since_last_save]

        assert_equal OK [r phmset dirty-tree p fields 1 f1 updated \
            branch:a fields 1 f one branch:b fields 1 f two]
        assert_equal 5 [s rdb_changes_since_last_save]
        assert_equal 2 [r phdel dirty-tree p f1 f2]
        assert_equal 7 [s rdb_changes_since_last_save]
        assert_equal 2 [r phpathdel dirty-tree branch: withsubpaths]
        assert_equal 9 [s rdb_changes_since_last_save]
        assert_equal 0 [r phpathdel dirty-tree branch: withsubpaths]
        assert_equal 9 [s rdb_changes_since_last_save]

        assert_equal OK [r phmset empty-dirty-tree a fields 1 f one \
            b fields 1 f two c fields 1 f three]
        r save
        assert_equal 3 [r phpathdel empty-dirty-tree {} withsubpaths]
        assert_equal 3 [s rdb_changes_since_last_save]
    }

    test {PHSCAN scans exact-path fields and MATCH filters names, not values} {
        r del scan-tree
        r phset scan-tree a fields 3 user:1 Alice user:2 Bob status user:fake
        r phset scan-tree ab fields 1 user:3 child
        set cursor 0
        set values {}
        set pages 0
        while 1 {
            lassign [r phscan scan-tree a $cursor match user:* count 1] cursor entries
            foreach {field value} $entries {dict set values $field $value}
            if {$cursor eq "0"} break
            assert {[incr pages] < 1000}
        }
        assert_equal {user:1 Alice user:2 Bob} [lsort -stride 2 $values]
        assert_equal {0 {}} [r phscan scan-tree a 0 match Alice]
        assert_equal {0 {}} [r phscan scan-tree missing 0]
        assert_equal {0 {}} [r phscan missing a 0]
        set path [binary format H* 0061ff]
        set field [binary format H* 6600ff]
        set value [binary format H* 7600ff]
        r phset scan-tree $path fields 1 $field $value
        assert_equal [list 0 [list $field $value]] [r phscan scan-tree $path 0 match "${field}*"]
        r phset scan-tree {} fields 1 root value
        assert_equal {0 {root value}} [r phscan scan-tree {} 0]
        assert_error ERR*invalid*cursor* {r phscan scan-tree a invalid}
        assert_error ERR*syntax* {r phscan scan-tree a 0 prefix a}
        assert_error ERR*syntax* {r phscan scan-tree a 0 withvalues}
        assert_error ERR*syntax* {r phscan scan-tree a 0 novalues}
        assert_error ERR*range* {r phscan scan-tree a 0 count 0}
        assert_error ERR*syntax* {r phscan scan-tree a 0 match}
        assert_error ERR*syntax* {r phpathscan scan-tree 0 withvalues}
        assert_error ERR*syntax* {r phpathscan scan-tree 0 match *}
        assert_error ERR*wrong*number* {r phscan scan-tree 0}
        assert_equal {scan-tree} [r command getkeys phscan scan-tree a 0 match user:*]
        assert_equal {scan-tree} [r command getkeys phpathscan scan-tree 0 prefix a]
    }

    test {Path Hash scans validate options even for missing keys and paths} {
        foreach command {
            {phscan scan-tree a 0} {phscan scan-tree missing 0}
            {phscan missing a 0} {phpathscan scan-tree 0} {phpathscan missing 0}
        } {
            set filter match
            if {[lindex $command 0] eq "phpathscan"} {set filter prefix}
            foreach options [list {count 1 count 5} [list $filter a $filter b] \
                {unknown value} {count} [list $filter]] {
                assert_error ERR*syntax* {r {*}$command {*}$options}
            }
            foreach count {0 -1} {
                assert_error ERR*range* {r {*}$command count $count}
            }
            assert_error ERR*integer* {r {*}$command count invalid}
        }
    }

    test {PHSCAN handles payload promotion between calls} {
        r del scan-tree
        set old_limit [lindex [r config get hash-max-listpack-entries] 1]
        test_defer {r config set hash-max-listpack-entries $old_limit}
        r config set hash-max-listpack-entries 4
        r phset scan-tree p fields 2 f0 v0 f1 v1
        # Compact payloads finish in one call, even with COUNT 1.
        lassign [r phscan scan-tree p 0 count 1] cursor entries
        assert_equal 0 $cursor
        assert_equal {f0 v0 f1 v1} [lsort -stride 2 $entries]
        for {set i 2} {$i < 100} {incr i} {
            r phset scan-tree p fields 1 f$i v$i
        }
        # Reuse the returned cursor after promotion; zero starts a new pass.
        set values {}
        set pages 0
        while 1 {
            lassign [r phscan scan-tree p $cursor count 1] cursor entries
            foreach {field value} $entries {dict set values $field $value}
            incr pages
            if {$cursor eq "0"} break
            assert {$pages < 1000}
        }
        assert {$pages > 1}
        assert_equal 100 [dict size $values]
        for {set i 0} {$i < 100} {incr i} {assert_equal v$i [dict get $values f$i]}
    }

    test {PHSCAN paginates large field maps without returning other paths} {
        r del scan-tree
        for {set i 0} {$i < 600} {incr i} {
            r phset scan-tree p fields 1 "field:$i" "value:$i"
        }
        r phset scan-tree px fields 1 unrelated hidden
        set cursor 0
        set values {}
        set pages 0
        while 1 {
            lassign [r phscan scan-tree p $cursor count 5] cursor entries
            incr pages
            foreach {field value} $entries {dict set values $field $value}
            if {$cursor eq "0"} break
            assert {$pages < 1000}
        }
        assert {$pages > 1}
        assert_equal 600 [dict size $values]
        for {set i 0} {$i < 600} {incr i} {
            assert_equal "value:$i" [dict get $values "field:$i"]
        }
        set cursor 0
        set fields {}
        while 1 {
            lassign [r phscan scan-tree p $cursor match field:59* count 2] cursor entries
            foreach {field value} $entries {dict set fields $field $value}
            if {$cursor eq "0"} break
        }
        assert_equal 11 [dict size $fields]
        set page [r phscan scan-tree p 0 count 1]
        assert {[lindex $page 0] ne "0"}
        r phdel scan-tree p
        assert_equal {0 {}} [r phscan scan-tree p [lindex $page 0]]
    }

    test {PHPATHSCAN handles binary prefixes and resumes after a deleted boundary} {
        r del scan-tree
        set prefix [binary format H* 00ff]
        foreach suffix {a b c} {r phset scan-tree "${prefix}$suffix" fields 1 f $suffix}
        lassign [r phpathscan scan-tree 0 prefix $prefix count 1] cursor entries
        assert_equal [list "${prefix}a"] $entries
        r phdel scan-tree "${prefix}a"
        assert_equal [list 0 [list "${prefix}b" "${prefix}c"]] \
            [r phpathscan scan-tree $cursor prefix $prefix count 2]
        assert_error {*cursor does not belong*} {r phpathscan scan-tree $cursor prefix other}
    }

    test {PHPATHSCAN uses an opaque cursor and traverses lexicographically} {
        r del tree
        foreach path {{} b aa a ab c} {r phset tree $path fields 1 f "v:$path"}
        set cursor 0
        set paths {}
        while 1 {
            set page [r phpathscan tree $cursor count 2]
            set cursor [lindex $page 0]
            foreach path [lindex $page 1] {lappend paths $path}
            if {$cursor eq "0"} break
        }
        assert_equal {{} a aa ab b c} $paths

        set prefixed [r phpathscan tree 0 prefix a count 100]
        assert_equal 0 [lindex $prefixed 0]
        assert_equal {a aa ab} [lindex $prefixed 1]
    }

    test {PHPATHSCAN ends the traversal without an extra empty call} {
        r del tree
        r phset tree a fields 1 f v
        r phset tree b fields 1 f v
        set page [r phpathscan tree 0 count 2]
        assert_equal 0 [lindex $page 0]
        assert_equal {a b} [lindex $page 1]

        set page [r phpathscan tree 0 count 1]
        assert_equal {a} [lindex $page 1]
        assert {[lindex $page 0] ne "0"}
        set page [r phpathscan tree [lindex $page 0] count 1]
        assert_equal {b} [lindex $page 1]
        assert_equal 0 [lindex $page 0]

        r phset tree ba fields 1 f v
        set page [r phpathscan tree 0 prefix b count 2]
        assert_equal {b ba} [lindex $page 1]
        assert_equal 0 [lindex $page 0]
    }

    test {Path Hash commands return WRONGTYPE consistently} {
        r set not-pathhash value
        foreach command {
            {phset not-pathhash p fields 1 f v}
            {phmset not-pathhash p fields 1 f v}
            {phget not-pathhash p f}
            {phmget not-pathhash p fields 1 f}
            {phgetall not-pathhash p}
            {phpathexists not-pathhash p}
            {phdel not-pathhash p}
            {phprefixes not-pathhash p}
            {phpathdel not-pathhash p withsubpaths}
            {phpathscan not-pathhash 0}
            {phscan not-pathhash p 0}
            {phpathcount not-pathhash}
        } {
            assert_error WRONGTYPE* {r {*}$command}
        }
    }

    test {Path Hash option syntax rejects ambiguous and invalid inputs} {
        r del tree
        assert_error ERR*syntax* {r phset tree p fnx fxx fields 1 f v}
        assert_error ERR*syntax* {r phset tree p fnx fnx fields 1 f v}
        assert_error ERR*syntax* {r phset tree p fields 2 f v}
        assert_error ERR*syntax* {r phset tree p fields 1 f v extra}
        assert_error ERR*value*out*range* {r phset tree p fields 0 f v}
        assert_error ERR*wrong*number* {r phmset tree p f v}
        assert_error ERR*value*out*range* {r phmset tree p fields 0 f v}
        assert_error ERR*syntax* {r phmset tree p fields 2 f v}
        assert_error ERR*syntax* {r phmset tree p fields 1 f v broken}
        assert_error ERR*wrong*number* {r phmget tree p f}
        assert_error ERR*value*out*range* {r phmget tree p fields 0 f}
        assert_error ERR*syntax* {r phmget tree p fields 2 f}
        assert_error ERR*syntax* {r phmget tree p fields 1 f broken}
        assert_equal 0 [r exists tree]
        assert_error ERR*syntax* {r phprefixes tree p length}
        assert_error ERR*range* {r phprefixes tree p fields 0}
        assert_error ERR*value*out*range* {r phprefixes tree p count 0}
        assert_error ERR*value*out*range* {r phprefixes tree p maxpathlen -1}
        assert_error ERR*syntax* {r phprefixes tree p fields 2 only-one}
        assert_error ERR*invalid*cursor* {r phpathscan tree invalid}
        assert_error ERR*syntax* {r phpathscan tree 0 count 1 count 2}
        assert_error ERR*value*out*range* {r phpathscan tree 0 count 0}
    }

    test {Path Hash commands are registered with the PH prefix} {
        set commands {phdel phget phgetall phmget phmset phpathcount phpathdel phpathexists phpathscan phprefixes phscan phset}
        assert_equal $commands [lsort [r command list filterby aclcat pathhash]]
        foreach command $commands {
            assert_no_match {*radix*} [string tolower [r command docs $command]]
            assert_equal $command [lindex [lindex [r command info $command] 0] 0]
            assert_equal pathhash [dict get [dict get [r command docs $command] $command] group]
            assert_match {*path hash*} [dict get [dict get [r command docs $command] $command] summary]
            set categories [lindex [lindex [r command info $command] 0] 6]
            assert {"@pathhash" in $categories}
            assert {"@path-hash" ni $categories}
            assert {"@radix" ni $categories}
            set old_command "rax[string range $command 2 end]"
            assert_equal {{}} [r command info $old_command]
        }
    }

    test {Path Hash ACL category grants and revokes access to PH commands} {
        with_cleanup {
            assert_equal OK [r acl setuser ph-user reset on nopass ~* +@pathhash]
            assert_equal OK [r acl dryrun ph-user phset tree a fields 1 f v]
            assert_equal OK [r acl dryrun ph-user phget tree a f]
            assert_equal OK [r acl dryrun ph-user phpathscan tree 0]
            assert_equal OK [r acl dryrun ph-user phscan tree a 0]
            assert_match {*no permissions*} [r acl dryrun ph-user set tree value]
            assert_equal OK [r acl setuser ph-user -@pathhash]
            assert_match {*no permissions*} [r acl dryrun ph-user phset tree a fields 1 f v]
            assert_error {*Unknown category*} {r acl cat radix}
            assert_error {*Unknown category*} {r acl cat path-hash}
        } {
            r acl deluser ph-user
        }
    }

    test {Command metadata, ACL category, RESP3, and transactions expose the native type} {
        assert_equal pathhash [dict get [dict get [r command docs phset] phset] group]
        assert_equal 9.2.0 [dict get [dict get [r command docs phset] phset] since]
        assert {[lsearch -exact [r command list filterby aclcat pathhash] phset] >= 0}
        assert {[lsearch -exact [r acl cat pathhash] phprefixes] >= 0}
        assert {[lsearch -exact [r acl cat pathhash] phmset] >= 0}
        assert {[lsearch -exact [r acl cat pathhash] phpathexists] >= 0}
        assert {[lsearch -exact [r command list filterby aclcat slow] phdel] >= 0}
        assert {[lsearch -exact [r command list filterby aclcat fast] phdel] < 0}
        r del tree
        r multi
        r phset tree a fields 1 f one
        r phset tree ab fields 1 f two
        assert_equal {OK OK} [r exec]
        assert_equal pathhash [r type tree]
        set scan [r scan 0 type pathhash count 100]
        assert {[lsearch -exact [lindex $scan 1] tree] >= 0}
        assert_equal $scan [r scan 0 type PATHHASH count 100]
        assert_error {*unknown type name*} {r scan 0 type radix}
        r hello 3
        assert_equal {2 1} [r phprefixes tree abc lengths]
        assert_equal [list two] [r phget tree ab f]
        r hello 2
    }

    test {RESP2 and RESP3 encode all PH payloads and field selections consistently} {
        r del tree
        r del missing
        r phset tree a fields 1 f1 v1
        foreach protocol {2 3} {
            # The harness overrides HELLO 2 when RESP3 is forced.
            if {$::force_resp3 && $protocol == 2} {continue}
            r hello $protocol
            set payload [list {*2} {$2} f1 {$2} v1]
            set empty {*0}
            set null {$-1}
            if {$protocol == 3} {
                lset payload 0 {%1}
                set empty {%0}
                set null {_}
            }
            set match [concat [list {*2} {$1} a] $payload]
            set fields [list {*2} {$1} a {*2} {$2} v1 $null]
            r readraw 1
            r deferred 1
            foreach {command expected} [list \
                {phget tree a f1} [list {*1} {$2} v1] \
                {phget tree a missing} [list {*1} $null] \
                {phget tree missing f1} [list {*1} $null] \
                {phget missing a f1} [list {*1} $null] \
                {phget tree a f1 missing f1} [list {*3} {$2} v1 $null {$2} v1] \
                {phmget tree a fields 1 f1} [list {*1} {$2} v1] \
                {phmget missing a fields 1 f1} [list {*1} $null] \
                {phgetall tree a} $payload \
                {phgetall tree missing} [list $empty] \
                {phgetall missing a} [list $empty] \
                {phprefixes tree a} [list {*1} {$1} a] \
                {phprefixes tree a lengths count 1 maxpathlen 1} [list {*1} {:1}] \
                {phpathscan tree 0 count 1} [list {*2} {$1} 0 {*1} {$1} a] \
                {phscan tree a 0} [list {*2} {$1} 0 {*2} {$2} f1 {$2} v1] \
                {phscan missing a 0} [list {*2} {$1} 0 {*0}] \
                {phprefixes tree a withvalues} [concat [list {*1}] $match] \
                {phprefixes tree a lengths withvalues} [concat [list {*1} {*2} {:1}] $payload] \
                {phprefixes tree a fields 1 f1} [list {*1} {*2} {$1} a {*1} {$2} v1] \
                {phprefixes tree a fields 2 f1 missing} [concat [list {*1}] $fields] \
                {phprefixes tree z withvalues} [list {*0}] \
                {phpathscan missing 0} [list {*2} {$1} 0 {*0}]] {
                r {*}$command
                foreach line $expected {
                    assert_equal $line [r read]
                }
            }
            r readraw 0
            r deferred 0
        }
        if {$::force_resp3} {
            r hello 3
        } else {
            r hello 2
        }
        assert_equal {f1 v1} [r phgetall tree a]
    } {} {resp3}

    test {PHGET distinguishes empty strings from missing fields inside arrays} {
        r del tree
        r phset tree a fields 1 f {}
        foreach protocol {2 3} {
            if {$::force_resp3 && $protocol == 2} {continue}
            r hello $protocol
            set null {$-1}
            if {$protocol == 3} {set null {_}}
            r readraw 1
            r deferred 1
            foreach {command expected} [list \
                {phget tree a f} [list {*1} {$0} {}] \
                {phget tree a f missing} [list {*2} {$0} {} $null]] {
                r {*}$command
                foreach line $expected {assert_equal $line [r read]}
            }
            r readraw 0
            r deferred 0
        }
        if {$::force_resp3} {r hello 3} else {r hello 2}
        assert_equal [list {} {}] [r phget tree a f missing]
    } {} {resp3}

    test {Path hash keyspace notifications use the p class} {
        assert_error {*Invalid event class character*} {r config set notify-keyspace-events Kr}
        r config set notify-keyspace-events Kp
        r del notify-tree
        set rd1 [valkey_deferring_client]
        with_cleanup {
            assert_equal {1} [psubscribe $rd1 *]
            assert_equal pK [lindex [r config get notify-keyspace-events] 1]
            r phset notify-tree a fields 1 f one
            r phmset notify-tree ab fields 1 f two
            r phdel notify-tree a
            r phpathdel notify-tree a withsubpaths
            assert_match "pmessage * __keyspace@*__:notify-tree phset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree phmset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree phdel" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree phpathdel" [$rd1 read]

            r config set notify-keyspace-events Kpg
            assert_equal gpK [lindex [r config get notify-keyspace-events] 1]
            r phset notify-tree a fields 1 f one
            r phdel notify-tree a
            assert_equal 0 [r exists notify-tree]
            assert_match "pmessage * __keyspace@*__:notify-tree phset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree phdel" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree del" [$rd1 read]

            r phset notify-tree a fields 1 f one
            r phset notify-tree ab fields 1 f two
            r phpathdel notify-tree {} withsubpaths
            assert_equal 0 [r exists notify-tree]
            assert_match "pmessage * __keyspace@*__:notify-tree phset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree phset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree phpathdel" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree del" [$rd1 read]
        } {
            catch {$rd1 close}
            r config set notify-keyspace-events {}
        }
    }

    test {UNLINK asynchronously frees a large path hash object} {
        r del tree
        for {set i 0} {$i < 100} {incr i} {
            r phset tree "path:$i" fields 1 field "value:$i"
        }
        assert_equal 100 [r phpathcount tree]
        assert_equal 1 [r unlink tree]
        assert_equal 0 [r exists tree]
        wait_for_condition 100 10 {
            [getInfoProperty [r info memory] lazyfree_pending_objects] == 0
        } else {
            fail "Path Hash object was not reclaimed by lazy free"
        }
    }

    test {COPY, DUMP/RESTORE, TTL, MEMORY USAGE, and DEBUG DIGEST support path hash values} {
        set tree {tree:{pathhash-copy}}
        set tree_copy {tree-copy:{pathhash-copy}}
        set tree_restored {tree-restored:{pathhash-copy}}
        r del $tree $tree_copy $tree_restored
        r phset $tree {} fields 1 root value
        r phset $tree abc fields 2 f1 v1 f2 v2
        assert_match {*encoding:pathhash*} [r debug object $tree]
        # DIGEST-VALUE includes whether a key has a TTL. Compare persistent
        # values below, while testing COPY's TTL preservation separately.
        set digest_before [r debug digest-value $tree]
        r pexpire $tree 60000
        assert {[r memory usage $tree] > 0}
        assert_equal 1 [r copy $tree $tree_copy]
        assert {[r pttl $tree_copy] > 0}
        assert_equal 1 [r persist $tree_copy]
        assert_equal {root value} [r phgetall $tree_copy {}]
        assert_equal 2 [r phpathcount $tree_copy]
        set dumped [r dump $tree]
        assert_equal OK [r restore $tree_restored 0 $dumped]
        assert_equal [r phprefixes $tree abc withvalues] [r phprefixes $tree_restored abc withvalues]
        assert_equal $digest_before [r debug digest-value $tree_copy]
        assert_equal $digest_before [r debug digest-value $tree_restored]

        # The digest must ignore insertion order, but include every path,
        # field and value. These changes preserve the number of entries.
        r del $tree_copy
        r phset $tree_copy abc fields 2 f2 v2 f1 v1
        r phset $tree_copy {} fields 1 root value
        assert_equal $digest_before [r debug digest-value $tree_copy]
        foreach {path field value} {abc f1 changed abc other v1 abcd f1 v1} {
            r restore $tree_restored 0 $dumped replace
            r phdel $tree_restored abc f1
            r phset $tree_restored $path fields 1 $field $value
            assert {$digest_before ne [r debug digest-value $tree_restored]}
        }
    } {} {needs:debug}

    test {Final Path Hash deletions remove the key, TTL and WATCH state} {
        set other [valkey_client]
        test_defer {$other close}
        foreach path [list {} p [binary format H* 00ff61]] {
            foreach field_count {2 600} {
                set fields {}
                set names {}
                for {set i 0} {$i < $field_count} {incr i} {
                    lappend fields f:$i v:$i
                    lappend names f:$i
                }
                foreach mode {fields path prefix all} {
                    r phset final-tree $path fields $field_count {*}$fields
                    r pexpire final-tree 60000
                    r watch final-tree
                    switch $mode {
                        fields {
                            assert_equal $field_count [$other phdel final-tree $path {*}$names missing f:0]
                        }
                        path {assert_equal 1 [$other phdel final-tree $path]}
                        prefix {assert_equal 1 [$other phpathdel final-tree $path withsubpaths]}
                        all {assert_equal 1 [$other phpathdel final-tree {} withsubpaths]}
                    }
                    r multi
                    r exists final-tree
                    assert_equal {} [r exec]
                    assert_equal 0 [r exists final-tree]
                    assert_equal none [r type final-tree]
                    assert_equal -2 [r pttl final-tree]
                    assert_equal 0 [r phpathcount final-tree]
                    assert_equal {} [r dump final-tree]
                    assert_equal 0 [r copy final-tree final-copy{final-tree}]
                    assert_equal 0 [r phdel final-tree $path]
                    assert_equal 0 [r phpathdel final-tree $path withsubpaths]
                    r set final-tree string
                    assert_equal string [r get final-tree]
                    assert_equal -1 [r pttl final-tree]
                    r del final-tree
                }
            }
        }
    }

    test {Partial Path Hash deletions do not emit a generic del notification} {
        r phmset partial-tree a fields 2 f one g two ab fields 1 f child keep fields 1 f keep
        set old_notify [lindex [r config get notify-keyspace-events] 1]
        test_defer {r config set notify-keyspace-events $old_notify}
        r config set notify-keyspace-events Kpg
        set subscriber [valkey_deferring_client]
        test_defer {$subscriber close}
        assert_equal {1} [psubscribe $subscriber *:partial-tree]
        foreach deletion {
            {phdel partial-tree a f}
            {phdel partial-tree a}
            {phpathdel partial-tree ab withsubpaths}
        } {
            assert_equal 1 [r {*}$deletion]
            assert_equal 1 [r exists partial-tree]
            assert_match "pmessage * __keyspace@*__:partial-tree [lindex $deletion 0]" [$subscriber read]
            # A following event exposes a spurious del without a timed read.
            r phset partial-tree keep fields 1 f keep
            assert_match "pmessage * __keyspace@*__:partial-tree phset" [$subscriber read]
        }
    }

    test {RDB reload restores path hash paths, binary data, root payload, and TTL} {
        r del tree
        set binary_path [binary format H* 000102ff]
        set binary_value [binary format H* 7600616cff]
        r phset tree {} fields 1 root root-value
        r phset tree $binary_path fields 1 field $binary_value
        r pexpire tree 60000
        r debug reload
        assert_equal pathhash [r type tree]
        assert_equal [list root-value] [r phget tree {} root]
        assert_equal [list $binary_value] [r phget tree $binary_path field]
        assert_equal 2 [r phpathcount tree]
        assert {[r pttl tree] > 0}
    } {} {needs:debug}

    test {RESTORE rejects malformed path hash payloads without destabilizing the server} {
        r debug set-skip-checksum-validation 1

        r phset legacy-source p fields 1 f v
        set dumped [r dump legacy-source]
        set empty_dump "[string index $dumped 0]\x00[string range $dumped end-9 end]"
        assert_error {*Bad data format*} {r restore legacy-empty 0 $empty_dump}
        assert_equal 0 [r exists legacy-empty]

        r del source
        r del corrupt
        r phset source p fields 1 f v
        set empty_payload [string replace [r dump source] 4 4 "\x00"]
        catch {r restore corrupt 0 $empty_payload} err
        assert_match {*Bad data format*} $err

        r del source
        r del corrupt
        r phset source a fields 1 f x
        r phset source b fields 1 f y
        set duplicate_path [r dump source]
        set duplicate_path [string replace $duplicate_path 10 10 [string index $duplicate_path 3]]
        catch {r restore corrupt 0 $duplicate_path} err
        assert_match {*Bad data format*} $err

        r del source
        r del corrupt
        r phset source p fields 1 a x
        r phset source p fields 1 b y
        set duplicate_field [r dump source]
        set duplicate_field [string replace $duplicate_field 10 10 [string index $duplicate_field 6]]
        catch {r restore corrupt 0 $duplicate_field} err
        assert_match {*Bad data format*} $err

        assert_equal PONG [r ping]
        assert_equal OK [r debug set-skip-checksum-validation 0]
    } {} {needs:debug}

    test {Large per-path payload promotes and survives RDB round trip} {
        r del tree
        for {set i 0} {$i < 600} {incr i} {
            r phset tree path fields 1 "field:$i" "value:$i"
        }
        assert_equal 1 [r phpathcount tree]
        assert_equal [list value:599] [r phget tree path field:599]
        r debug reload
        assert_equal [list value:0] [r phget tree path field:0]
        assert_equal [list value:599] [r phget tree path field:599]
        assert_equal 1200 [llength [r phgetall tree path]]
    } {} {needs:debug}

    test {valkey-check-rdb validates and reports the native path hash type} {
        r del tree
        r phset tree {} fields 1 root value
        r phset tree abc fields 1 f v
        r save
        set dir [lindex [r config get dir] 1]
        set filename [lindex [r config get dbfilename] 1]
        set output [exec $::VALKEY_CHECK_RDB_BIN [file join $dir $filename] --stats --format info]
        assert_match {*RDB looks OK*} $output
        assert_match {*pathhash*} $output
        assert_no_match {*radix*} [string tolower $output]
    } {} {external:skip}
}

start_server {tags {pathhash needs:debug} overrides {appendonly yes aof-use-rdb-preamble no}} {
    test {AOF rewrite and reload preserve populated Path Hash values and final deletion} {
        r phset tree {} fields 1 root value
        r phset tree abc fields 2 f1 v1 f2 v2
        r phset tree abcd fields 1 child v3
        r phdel tree abc f1
        r phset empty-tree path fields 1 field value
        r phdel empty-tree path
        r debug loadaof
        assert_equal 0 [r exists empty-tree]
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
        assert_equal 3 [r phpathcount tree]
        assert_equal [list value] [r phget tree {} root]
        assert_equal {{} v2} [r phget tree abc f1 f2]
        assert_equal [list v3] [r phget tree abcd child]
        assert_equal 0 [r exists empty-tree]
        assert_equal none [r type empty-tree]
        assert_equal 0 [r phpathcount empty-tree]
    }
}

start_server {tags {pathhash external:skip}} {
    start_server {tags {pathhash external:skip}} {
        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        set replica [srv 0 client]

        test {PHSCAN fully iterates a large payload on a read-only replica} {
            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 100 {[s 0 master_link_status] eq {up}} else {
                fail "Replication not started"
            }
            for {set i 0} {$i < 600} {incr i} {
                $primary phset replica-scan p fields 1 field:$i value:$i
            }
            $primary phset replica-scan px fields 1 unrelated hidden
            wait_for_ofs_sync $primary $replica
            set cursor 0
            set values {}
            set pages 0
            while 1 {
                lassign [$replica phscan replica-scan p $cursor match field:5* count 2] cursor entries
                foreach {field value} $entries {dict set values $field $value}
                incr pages
                if {$cursor eq "0"} break
                assert {$pages < 1000}
            }
            assert {$pages > 1}
            assert_equal 111 [dict size $values]
            foreach field [dict keys $values] {
                assert_match field:5* $field
                assert_equal value:[string range $field 6 end] [dict get $values $field]
            }
        }

        test {PHPATHDEL exact deletion replicates binary paths without removing descendants} {
            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 100 {[s 0 master_link_status] eq {up}} else {
                fail "Replication not started"
            }
            set path [binary format H* 0061ff]
            set child "${path}\x00"
            $primary phmset exact-tree $path fields 2 f one g two \
                $child fields 1 f child {} fields 1 f root
            wait_for_ofs_sync $primary $replica
            assert_equal 3 [$replica phpathcount exact-tree]
            assert_equal 1 [$primary phpathdel exact-tree $path]
            assert_equal 0 [$primary phpathdel exact-tree $path]
            wait_for_ofs_sync $primary $replica
            assert_equal 0 [$replica phpathexists exact-tree $path]
            assert_equal {child} [$replica phget exact-tree $child f]
            assert_equal {root} [$replica phget exact-tree {} f]
            assert_equal 2 [$replica phpathcount exact-tree]
            assert_equal 1 [$primary phpathdel exact-tree {}]
            wait_for_ofs_sync $primary $replica
            assert_equal 1 [$replica phpathcount exact-tree]
            assert_equal {child} [$replica phget exact-tree $child f]
        }

        test {Path Hash writes, conditional no-ops, and subtree deletes replicate} {
            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started"
            }

            $primary phset tree a fields 1 f one
            $primary phmset tree ab fields 1 f two abc fields 1 f three
            assert_equal {} [$primary phset tree a fnx fields 1 f ignored]
            $primary phpathdel tree ab withsubpaths
            $primary phset empty-tree path fields 1 field value
            $primary phdel empty-tree path
            foreach deletion {
                {phdel final-tree p f}
                {phdel final-tree p}
                {phpathdel final-tree p withsubpaths}
                {phpathdel final-tree {} withsubpaths}
            } {
                $primary phset final-tree p fields 1 f v
                $primary pexpire final-tree 60000
                $primary {*}$deletion
                wait_for_ofs_sync $primary $replica
                assert_equal 0 [$replica exists final-tree]
                assert_equal -2 [$replica pttl final-tree]
            }
            wait_for_ofs_sync $primary $replica
            assert_equal 1 [$replica phpathcount tree]
            assert_equal [list one] [$replica phget tree a f]
            assert_equal [list {}] [$replica phget tree ab f]
            assert_equal 0 [$replica exists empty-tree]
            assert_equal none [$replica type empty-tree]
            assert_equal 0 [$replica phpathcount empty-tree]
        }
    }
}
