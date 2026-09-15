start_server {tags {radix}} {
    test {PHSET creates a native radix object and exact reads are binary safe} {
        set path [binary format H* 0001ff]
        set field [binary format H* 660069656c64]
        set value [binary format H* 7600616c7565ff]
        assert_equal OK [r phset tree $path fields 1 $field $value]
        assert_equal path-hash [r type tree]
        assert_equal radix [r object encoding tree]
        assert_equal 1 [r phcard tree]
        assert_equal $value [r phget tree $path $field]
        assert_equal {} [r phget tree $path missing]
        assert_equal [list $value {}] [r phget tree $path $field missing]
        assert_equal 0 [r phcard missing]
    }

    test {PHSET FNX and FXX atomically apply to the complete field group} {
        r del tree
        assert_equal {} [r phset tree path fxx fields 2 f v second two]
        assert_equal 0 [r exists tree]
        assert_equal OK [r phset tree path fnx fields 2 f v second two]
        assert_equal {} [r phset tree path fnx fields 2 f replacement missing added]
        assert_equal v [r phget tree path f]
        assert_equal {} [r phget tree path missing]
        assert_equal OK [r phset tree path fnx fields 1 third three]
        assert_equal three [r phget tree path third]
        assert_equal {} [r phset tree path fxx fields 2 f replacement missing added]
        assert_equal v [r phget tree path f]
        assert_equal {} [r phget tree path missing]
        assert_equal OK [r phset tree path fxx fields 2 f replacement second updated]
        assert_equal replacement [r phget tree path f]
        assert_equal updated [r phget tree path second]
        assert_equal OK [r phset tree path fields 2 duplicate first duplicate last]
        assert_equal last [r phget tree path duplicate]
        assert_equal 1 [r phcard tree]
    }

    test {Exact path operations do not confuse ancestors and descendants} {
        r del tree
        r phmset tree a fields 1 f one ab fields 1 f two abc fields 1 f three
        assert_equal one [r phget tree a f]
        assert_equal two [r phget tree ab f]
        assert_equal three [r phget tree abc f]
        assert_equal {} [r phget tree abcd f]
        assert_equal 3 [r phcard tree]
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

    test {PHEXISTS checks exact logical paths only} {
        r del tree
        r phmset tree ab fields 1 f one abcd fields 1 f two
        assert_equal 0 [r phexists tree a]
        assert_equal 1 [r phexists tree ab]
        assert_equal 0 [r phexists tree abc]
        assert_equal 1 [r phexists tree abcd]
        assert_equal 0 [r phexists missing ab]
    }

    test {PHLONGEST handles root, compressed edges, lengths, values, and field filters} {
        r del tree
        r phset tree {} fields 1 root root-value
        r phset tree a fields 1 f v-a
        r phset tree abc fields 2 f1 v1 f2 v2
        r phset tree abcdef fields 1 f deep
        assert_equal abc [r phlongest tree abczzz]
        assert_equal 3 [r phlongest tree abczzz length]
        assert_equal [list abc [list v1 {} v2]] [r phlongest tree abczzz fields 3 f1 missing f2]
        set withvalues [r phlongest tree abczzz withvalues]
        assert_equal abc [lindex $withvalues 0]
        set payload [dict create {*}[lindex $withvalues 1]]
        assert_equal v1 [dict get $payload f1]
        assert_equal v2 [dict get $payload f2]
        assert_equal {} [r phlongest tree zzz]
        assert_equal 0 [r phlongest tree zzz length]
        assert_equal {} [r phlongest missing anything]
    }

    test {PHLONGEST replies null when no stored path prefixes the query} {
        r del tree
        r phset tree abc fields 1 f v
        set nullres {$-1}
        if {$::force_resp3} {
            set nullres {_}
        }
        r readraw 1
        r deferred 1
        r phlongest tree zzz
        assert_equal [r read] $nullres
        r phlongest tree zzz length
        assert_equal [r read] $nullres
        r readraw 0
        r deferred 0
        # A stored empty root path is a real match of length 0, which the client
        # renders exactly like the null reply above.
        r phset tree {} fields 1 root v
        assert_equal {} [r phlongest tree zzz]
        assert_equal 0 [r phlongest tree zzz length]
    }

    test {PHPREFIXES orders ancestors and applies MAXLEN before deepest COUNT} {
        r del tree
        foreach path {{} a ab abc abcd} {
            r phset tree $path fields 1 f "value:$path"
        }
        assert_equal {0 1 2 3 4} [r phprefixes tree abcde lengths]
        assert_equal {2 3} [r phprefixes tree abcde lengths count 2 maxlen 3]
        assert_equal [list [list 2 [list value:ab]] [list 3 [list value:abc]]] \
            [r phprefixes tree abcde lengths fields 1 f count 2 maxlen 3]
        assert_equal [list {}] [r phprefixes tree zzz maxlen 0]
        assert_equal [list {}] [r phprefixes tree zzz]
    }

    test {PHPREFIXES validates COUNT and MAXLEN against the server long range} {
        r del tree
        r del missing
        foreach path {{} a ab} {
            r phset tree $path fields 1 f v
        }
        set long_max [expr {(1 << ([s arch_bits] - 1)) - 1}]
        foreach option {count maxlen} {
            assert_equal {0 1 2} [r phprefixes tree abc lengths $option $long_max]
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
                    assert_equal {0 1 2} [r phprefixes tree abc lengths $option $value]
                }
            }
            assert_error ERR*syntax* {r phprefixes tree abc $option 1 $option 2}
            assert_error ERR*syntax* {r phprefixes tree abc $option}
        }
        assert_error ERR*range* {r phprefixes tree abc count 0}
        assert_equal {2} [r phprefixes tree abc lengths count 1]
        assert_equal {0} [r phprefixes tree abc lengths maxlen 0]
        assert_equal {0 1 2} [r phprefixes tree abc lengths]
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
        assert_equal {0 1 2 4} [r phprefixes tree $query lengths]
        assert_equal 4 [r phlongest tree $query length]
        assert_equal 2 [r phdelprefix tree $p1]
        assert_equal [list {} $p0 $sibling] [lindex [r phscan tree 0 count 100] 1]
    }

    test {PHDEL removes fields, prunes empty payloads, and preserves descendants} {
        r del tree
        r phset tree a fields 2 f1 v1 f2 v2
        r phset tree ab fields 1 f child
        assert_equal 1 [r phdel tree a missing f1 missing]
        assert_equal {{} v2} [r phget tree a f1 f2]
        assert_equal 1 [r phdel tree a f2]
        assert_equal 1 [r phcard tree]
        assert_equal child [r phget tree ab f]
        assert_equal 1 [r phdel tree ab]
        assert_equal 1 [r exists tree]
        assert_equal path-hash [r type tree]
        assert_equal 0 [r phcard tree]
        assert_equal 0 [r phdel tree ab]
    }

    test {PHDELPREFIX deletes only descendants and empty prefix clears the tree} {
        r del tree
        foreach path {a ab abc ac b ba} {r phset tree $path fields 1 f $path}
        assert_equal 2 [r phdelprefix tree ab]
        set result [r phscan tree 0 count 100]
        assert_equal 0 [lindex $result 0]
        assert_equal {a ac b ba} [lindex $result 1]
        assert_equal 4 [r phdelprefix tree {}]
        assert_equal 1 [r exists tree]
        assert_equal path-hash [r type tree]
        assert_equal 0 [r phcard tree]
        assert_equal 0 [r phdelprefix tree anything]
    }

    test {PHDELPREFIX deletes matching paths in fixed-size chunks} {
        r del tree
        set assignments {}
        for {set i 0} {$i < 600} {incr i} {
            lappend assignments "delete:$i" fields 1 f "value:$i"
        }
        lappend assignments keep:a fields 1 f one keep:b fields 1 f two keep:c fields 1 f three
        assert_equal OK [r phmset tree {*}$assignments]
        assert_equal 603 [r phcard tree]
        assert_equal 600 [r phdelprefix tree delete:]
        assert_equal 3 [r phcard tree]
        assert_equal {keep:a keep:b keep:c} [lindex [r phscan tree 0 count 100] 1]
    }

    test {Radix write commands account server dirty by logical mutations} {
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
        assert_equal 2 [r phdelprefix dirty-tree branch:]
        assert_equal 9 [s rdb_changes_since_last_save]
        assert_equal 0 [r phdelprefix dirty-tree branch:]
        assert_equal 9 [s rdb_changes_since_last_save]

        assert_equal OK [r phmset empty-dirty-tree a fields 1 f one \
            b fields 1 f two c fields 1 f three]
        r save
        assert_equal 3 [r phdelprefix empty-dirty-tree {}]
        assert_equal 3 [s rdb_changes_since_last_save]
    }

    test {PHSCAN uses an opaque cursor and traverses lexicographically} {
        r del tree
        foreach path {{} b aa a ab c} {r phset tree $path fields 1 f "v:$path"}
        set cursor 0
        set paths {}
        while 1 {
            set page [r phscan tree $cursor count 2]
            set cursor [lindex $page 0]
            foreach path [lindex $page 1] {lappend paths $path}
            if {$cursor eq "0"} break
        }
        assert_equal {{} a aa ab b c} $paths

        set prefixed [r phscan tree 0 prefix a count 100 withvalues]
        assert_equal 0 [lindex $prefixed 0]
        assert_equal {a aa ab} [lmap entry [lindex $prefixed 1] {lindex $entry 0}]
        foreach entry [lindex $prefixed 1] {
            assert_equal "v:[lindex $entry 0]" [dict get [dict create {*}[lindex $entry 1]] f]
        }
    }

    test {PHSCAN ends the traversal without an extra empty call} {
        r del tree
        r phset tree a fields 1 f v
        r phset tree b fields 1 f v
        set page [r phscan tree 0 count 2]
        assert_equal 0 [lindex $page 0]
        assert_equal {a b} [lindex $page 1]

        set page [r phscan tree 0 count 1]
        assert_equal {a} [lindex $page 1]
        assert {[lindex $page 0] ne "0"}
        set page [r phscan tree [lindex $page 0] count 1]
        assert_equal {b} [lindex $page 1]
        assert_equal 0 [lindex $page 0]

        r phset tree ba fields 1 f v
        set page [r phscan tree 0 prefix b count 2]
        assert_equal {b ba} [lindex $page 1]
        assert_equal 0 [lindex $page 0]
    }

    test {Radix commands return WRONGTYPE consistently} {
        r set notradix value
        foreach command {
            {phset notradix p fields 1 f v}
            {phmset notradix p fields 1 f v}
            {phget notradix p f}
            {phmget notradix p fields 1 f}
            {phgetall notradix p}
            {phexists notradix p}
            {phdel notradix p}
            {phlongest notradix p}
            {phprefixes notradix p}
            {phdelprefix notradix p}
            {phscan notradix 0}
            {phcard notradix}
        } {
            assert_error WRONGTYPE* {r {*}$command}
        }
    }

    test {Radix option syntax rejects ambiguous and invalid inputs} {
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
        assert_error ERR*syntax* {r phlongest tree p withvalues fields 1 f}
        assert_error ERR*syntax* {r phlongest tree p lengths}
        assert_error ERR*range* {r phlongest tree p fields 0}
        assert_error ERR*syntax* {r phprefixes tree p length}
        assert_error ERR*range* {r phprefixes tree p fields 0}
        assert_error ERR*value*out*range* {r phprefixes tree p count 0}
        assert_error ERR*value*out*range* {r phprefixes tree p maxlen -1}
        assert_error ERR*syntax* {r phprefixes tree p fields 2 only-one}
        assert_error ERR*invalid*cursor* {r phscan tree invalid}
        assert_error ERR*syntax* {r phscan tree 0 count 1 count 2}
        assert_error ERR*value*out*range* {r phscan tree 0 count 0}
    }

    test {Path Hash commands are registered with the PH prefix} {
        set commands {phcard phdel phdelprefix phexists phget phgetall phlongest phmget phmset phprefixes phscan phset}
        assert_equal $commands [lsort [r command list filterby aclcat path-hash]]
        foreach command $commands {
            assert_equal $command [lindex [lindex [r command info $command] 0] 0]
            assert_equal path-hash [dict get [dict get [r command docs $command] $command] group]
            assert_match {*path hash*} [dict get [dict get [r command docs $command] $command] summary]
            set categories [lindex [lindex [r command info $command] 0] 6]
            assert {"@path-hash" in $categories}
            assert {"@radix" ni $categories}
            set old_command "rax[string range $command 2 end]"
            assert_equal {{}} [r command info $old_command]
        }
    }

    test {Path Hash ACL category grants and revokes access to PH commands} {
        with_cleanup {
            assert_equal OK [r acl setuser ph-user reset on nopass ~* +@path-hash]
            assert_equal OK [r acl dryrun ph-user phset tree a fields 1 f v]
            assert_equal OK [r acl dryrun ph-user phget tree a f]
            assert_equal OK [r acl dryrun ph-user phscan tree 0]
            assert_match {*no permissions*} [r acl dryrun ph-user set tree value]
            assert_equal OK [r acl setuser ph-user -@path-hash]
            assert_match {*no permissions*} [r acl dryrun ph-user phset tree a fields 1 f v]
            assert_error {*Unknown category*} {r acl cat radix}
        } {
            r acl deluser ph-user
        }
    }

    test {Command metadata, ACL category, RESP3, and transactions expose the native type} {
        assert_equal path-hash [dict get [dict get [r command docs phset] phset] group]
        assert_equal 9.2.0 [dict get [dict get [r command docs phset] phset] since]
        assert {[lsearch -exact [r command list filterby aclcat path-hash] phset] >= 0}
        assert {[lsearch -exact [r acl cat path-hash] phprefixes] >= 0}
        assert {[lsearch -exact [r acl cat path-hash] phmset] >= 0}
        assert {[lsearch -exact [r acl cat path-hash] phexists] >= 0}
        assert {[lsearch -exact [r command list filterby aclcat slow] phdel] >= 0}
        assert {[lsearch -exact [r command list filterby aclcat fast] phdel] < 0}
        r del tree
        r multi
        r phset tree a fields 1 f one
        r phset tree ab fields 1 f two
        assert_equal {OK OK} [r exec]
        assert_equal path-hash [r type tree]
        set scan [r scan 0 type path-hash count 100]
        assert {[lsearch -exact [lindex $scan 1] tree] >= 0}
        assert_equal $scan [r scan 0 type PATH-HASH count 100]
        assert_error {*unknown type name*} {r scan 0 type radix}
        r hello 3
        assert_equal {1 2} [r phprefixes tree abc lengths]
        assert_equal two [r phget tree ab f]
        r hello 2
    }

    test {RESP2 and RESP3 encode all PH payloads and field selections consistently} {
        r del tree
        r del missing
        r phset tree a fields 1 f1 v1
        foreach protocol {2 3} {
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
                {phgetall tree a} $payload \
                {phgetall tree missing} [list $empty] \
                {phgetall missing a} [list $empty] \
                {phlongest tree a withvalues} $match \
                {phlongest tree a length withvalues} [concat [list {*2} {:1}] $payload] \
                {phprefixes tree a withvalues} [concat [list {*1}] $match] \
                {phprefixes tree a lengths withvalues} [concat [list {*1} {*2} {:1}] $payload] \
                {phscan tree 0 withvalues} [concat [list {*2} {$1} 0 {*1}] $match] \
                {phlongest tree a fields 2 f1 missing} $fields \
                {phprefixes tree a fields 2 f1 missing} [concat [list {*1}] $fields] \
                {phlongest tree z withvalues} [list $null] \
                {phprefixes tree z withvalues} [list {*0}] \
                {phscan missing 0 withvalues} [list {*2} {$1} 0 {*0}]] {
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

    test {Radix keyspace notifications use the radix class} {
        r config set notify-keyspace-events Kr
        r del notify-tree
        set rd1 [valkey_deferring_client]
        with_cleanup {
            assert_equal {1} [psubscribe $rd1 *]
            assert_equal rK [lindex [r config get notify-keyspace-events] 1]
            r phset notify-tree a fields 1 f one
            r phmset notify-tree ab fields 1 f two
            r phdel notify-tree a
            r phdelprefix notify-tree a
            assert_match "pmessage * __keyspace@*__:notify-tree phset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree phmset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree phdel" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree phdelprefix" [$rd1 read]

            r config set notify-keyspace-events Krg
            assert_equal grK [lindex [r config get notify-keyspace-events] 1]
            r phset notify-tree a fields 1 f one
            r phdel notify-tree a
            assert_equal 1 [r exists notify-tree]
            r del notify-tree
            assert_match "pmessage * __keyspace@*__:notify-tree phset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree phdel" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree del" [$rd1 read]

            r phset notify-tree a fields 1 f one
            r phset notify-tree ab fields 1 f two
            r phdelprefix notify-tree {}
            assert_equal 1 [r exists notify-tree]
            r del notify-tree
            assert_match "pmessage * __keyspace@*__:notify-tree phset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree phset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree phdelprefix" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree del" [$rd1 read]
        } {
            catch {$rd1 close}
            r config set notify-keyspace-events {}
        }
    }

    test {UNLINK asynchronously frees a large radix object} {
        r del tree
        for {set i 0} {$i < 100} {incr i} {
            r phset tree "path:$i" fields 1 field "value:$i"
        }
        assert_equal 100 [r phcard tree]
        assert_equal 1 [r unlink tree]
        assert_equal 0 [r exists tree]
        wait_for_condition 100 10 {
            [getInfoProperty [r info memory] lazyfree_pending_objects] == 0
        } else {
            fail "Radix object was not reclaimed by lazy free"
        }
    }

    test {COPY, DUMP/RESTORE, TTL, MEMORY USAGE, and DEBUG DIGEST support radix values} {
        set tree {tree:{radix-copy}}
        set tree_copy {tree-copy:{radix-copy}}
        set tree_restored {tree-restored:{radix-copy}}
        r del $tree $tree_copy $tree_restored
        r phset $tree {} fields 1 root value
        r phset $tree abc fields 2 f1 v1 f2 v2
        r pexpire $tree 60000
        set digest_before [r debug digest]
        assert {[r memory usage $tree] > 0}
        assert_equal 1 [r copy $tree $tree_copy]
        assert {[r pttl $tree_copy] > 0}
        assert_equal {root value} [r phgetall $tree_copy {}]
        assert_equal 2 [r phcard $tree_copy]
        set dumped [r dump $tree]
        assert_equal OK [r restore $tree_restored 0 $dumped]
        assert_equal [r phprefixes $tree abc withvalues] [r phprefixes $tree_restored abc withvalues]
        assert {$digest_before ne ""}
    } {} {needs:debug}

    test {Empty Radix survives COPY, DUMP/RESTORE, and RDB reload} {
        set empty_tree {empty-tree:{radix-empty}}
        set empty_copy {empty-copy:{radix-empty}}
        set empty_restored {empty-restored:{radix-empty}}
        r del $empty_tree $empty_copy $empty_restored
        r phset $empty_tree path fields 1 field value
        assert_equal 1 [r phdel $empty_tree path]
        assert_equal 1 [r exists $empty_tree]
        assert_equal path-hash [r type $empty_tree]
        assert_equal 0 [r phcard $empty_tree]

        assert_equal 1 [r copy $empty_tree $empty_copy]
        assert_equal path-hash [r type $empty_copy]
        assert_equal 0 [r phcard $empty_copy]

        set dumped [r dump $empty_tree]
        assert_equal OK [r restore $empty_restored 0 $dumped]
        assert_equal path-hash [r type $empty_restored]
        assert_equal 0 [r phcard $empty_restored]

        r debug reload
        foreach key [list $empty_tree $empty_copy $empty_restored] {
            assert_equal 1 [r exists $key]
            assert_equal path-hash [r type $key]
            assert_equal 0 [r phcard $key]
            assert_equal {0 {}} [r phscan $key 0]
        }
        assert_equal 1 [r del $empty_tree]
        assert_equal 1 [r unlink $empty_copy]
        assert_equal 0 [r exists $empty_tree]
        assert_equal 0 [r exists $empty_copy]
        assert_equal 1 [r exists $empty_restored]
    } {} {needs:debug}

    test {RDB reload restores radix paths, binary data, root payload, and TTL} {
        r del tree
        set binary_path [binary format H* 000102ff]
        set binary_value [binary format H* 7600616cff]
        r phset tree {} fields 1 root root-value
        r phset tree $binary_path fields 1 field $binary_value
        r pexpire tree 60000
        r debug reload
        assert_equal path-hash [r type tree]
        assert_equal root-value [r phget tree {} root]
        assert_equal $binary_value [r phget tree $binary_path field]
        assert_equal 2 [r phcard tree]
        assert {[r pttl tree] > 0}
    } {} {needs:debug}

    test {RESTORE rejects malformed radix payloads without destabilizing the server} {
        r debug set-skip-checksum-validation 1

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
        assert_equal 1 [r phcard tree]
        assert_equal value:599 [r phget tree path field:599]
        r debug reload
        assert_equal value:0 [r phget tree path field:0]
        assert_equal value:599 [r phget tree path field:599]
        assert_equal 1200 [llength [r phgetall tree path]]
    } {} {needs:debug}

    test {valkey-check-rdb validates and reports the native radix type} {
        r del tree
        r phset tree {} fields 1 root value
        r phset tree abc fields 1 f v
        r save
        set dir [lindex [r config get dir] 1]
        set filename [lindex [r config get dbfilename] 1]
        set output [exec $::VALKEY_CHECK_RDB_BIN [file join $dir $filename] --stats --format info]
        assert_match {*RDB looks OK*} $output
        assert_match {*path-hash*} $output
    } {} {external:skip}
}

start_server {tags {radix needs:debug} overrides {appendonly yes aof-use-rdb-preamble no}} {
    test {AOF rewrite and reload preserve populated and empty Radix values} {
        r phset tree {} fields 1 root value
        r phset tree abc fields 2 f1 v1 f2 v2
        r phset tree abcd fields 1 child v3
        r phdel tree abc f1
        r phset empty-tree path fields 1 field value
        r phdel empty-tree path
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
        assert_equal 3 [r phcard tree]
        assert_equal value [r phget tree {} root]
        assert_equal {{} v2} [r phget tree abc f1 f2]
        assert_equal v3 [r phget tree abcd child]
        assert_equal 1 [r exists empty-tree]
        assert_equal path-hash [r type empty-tree]
        assert_equal 0 [r phcard empty-tree]
    }
}

start_server {tags {radix external:skip}} {
    start_server {tags {radix external:skip}} {
        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        set replica [srv 0 client]

        test {Radix writes, conditional no-ops, and subtree deletes replicate} {
            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started"
            }

            $primary phset tree a fields 1 f one
            $primary phmset tree ab fields 1 f two abc fields 1 f three
            assert_equal {} [$primary phset tree a fnx fields 1 f ignored]
            $primary phdelprefix tree ab
            $primary phset empty-tree path fields 1 field value
            $primary phdel empty-tree path
            wait_for_ofs_sync $primary $replica
            assert_equal 1 [$replica phcard tree]
            assert_equal one [$replica phget tree a f]
            assert_equal {} [$replica phget tree ab f]
            assert_equal 1 [$replica exists empty-tree]
            assert_equal path-hash [$replica type empty-tree]
            assert_equal 0 [$replica phcard empty-tree]
        }
    }
}
