start_server {tags {radix}} {
    test {RAXSET creates a native radix object and exact reads are binary safe} {
        set path [binary format H* 0001ff]
        set field [binary format H* 660069656c64]
        set value [binary format H* 7600616c7565ff]
        assert_equal OK [r raxset tree $path fields 1 $field $value]
        assert_equal radix [r type tree]
        assert_equal radix [r object encoding tree]
        assert_equal 1 [r raxcard tree]
        assert_equal $value [r raxget tree $path $field]
        assert_equal {} [r raxget tree $path missing]
        assert_equal [list $value {}] [r raxget tree $path $field missing]
        assert_equal 0 [r raxcard missing]
    }

    test {RAXSET FNX and FXX atomically apply to the complete field group} {
        r del tree
        assert_equal {} [r raxset tree path fxx fields 2 f v second two]
        assert_equal 0 [r exists tree]
        assert_equal OK [r raxset tree path fnx fields 2 f v second two]
        assert_equal {} [r raxset tree path fnx fields 2 f replacement missing added]
        assert_equal v [r raxget tree path f]
        assert_equal {} [r raxget tree path missing]
        assert_equal OK [r raxset tree path fnx fields 1 third three]
        assert_equal three [r raxget tree path third]
        assert_equal {} [r raxset tree path fxx fields 2 f replacement missing added]
        assert_equal v [r raxget tree path f]
        assert_equal {} [r raxget tree path missing]
        assert_equal OK [r raxset tree path fxx fields 2 f replacement second updated]
        assert_equal replacement [r raxget tree path f]
        assert_equal updated [r raxget tree path second]
        assert_equal OK [r raxset tree path fields 2 duplicate first duplicate last]
        assert_equal last [r raxget tree path duplicate]
        assert_equal 1 [r raxcard tree]
    }

    test {Exact path operations do not confuse ancestors and descendants} {
        r del tree
        r raxmset tree a fields 1 f one ab fields 1 f two abc fields 1 f three
        assert_equal one [r raxget tree a f]
        assert_equal two [r raxget tree ab f]
        assert_equal three [r raxget tree abc f]
        assert_equal {} [r raxget tree abcd f]
        assert_equal 3 [r raxcard tree]
    }

    test {RAXGET, RAXMGET, and RAXGETALL preserve field semantics} {
        r del tree
        r raxset tree path fields 2 f1 v1 f2 v2
        r raxmset tree other fields 2 f1 ignored f1 other-v1 third fields 1 f3 v3
        set values [dict create {*}[r raxgetall tree path]]
        assert_equal v1 [dict get $values f1]
        assert_equal v2 [dict get $values f2]
        assert_equal {v2 {} v1} [r raxget tree path f2 missing f1]
        assert_equal {v1 v2 other-v1 v3 {} v1} \
            [r raxmget tree path fields 2 f1 f2 other fields 1 f1 \
                third fields 1 f3 missing fields 1 f path fields 1 f1]
        assert_equal {v2 {} v1} [r raxmget tree path fields 3 f2 missing f1]
        assert_equal {{} {}} [r raxget missing path f1 f2]
        assert_equal {} [r raxgetall tree missing]
    }

    test {RAXEXISTS checks exact logical paths only} {
        r del tree
        r raxmset tree ab fields 1 f one abcd fields 1 f two
        assert_equal 0 [r raxexists tree a]
        assert_equal 1 [r raxexists tree ab]
        assert_equal 0 [r raxexists tree abc]
        assert_equal 1 [r raxexists tree abcd]
        assert_equal 0 [r raxexists missing ab]
    }

    test {RAXLONGEST handles root, compressed edges, lengths, values, and field filters} {
        r del tree
        r raxset tree {} fields 1 root root-value
        r raxset tree a fields 1 f v-a
        r raxset tree abc fields 2 f1 v1 f2 v2
        r raxset tree abcdef fields 1 f deep
        assert_equal abc [r raxlongest tree abczzz]
        assert_equal 3 [r raxlongest tree abczzz length]
        assert_equal [list abc [list v1 {} v2]] [r raxlongest tree abczzz fields 3 f1 missing f2]
        set withvalues [r raxlongest tree abczzz withvalues]
        assert_equal abc [lindex $withvalues 0]
        set payload [dict create {*}[lindex $withvalues 1]]
        assert_equal v1 [dict get $payload f1]
        assert_equal v2 [dict get $payload f2]
        assert_equal {} [r raxlongest tree zzz]
        assert_equal 0 [r raxlongest tree zzz length]
        assert_equal {} [r raxlongest missing anything]
    }

    test {RAXLONGEST replies null when no stored path prefixes the query} {
        r del tree
        r raxset tree abc fields 1 f v
        set nullres {$-1}
        if {$::force_resp3} {
            set nullres {_}
        }
        r readraw 1
        r deferred 1
        r raxlongest tree zzz
        assert_equal [r read] $nullres
        r raxlongest tree zzz length
        assert_equal [r read] $nullres
        r readraw 0
        r deferred 0
        # A stored empty root path is a real match of length 0, which the client
        # renders exactly like the null reply above.
        r raxset tree {} fields 1 root v
        assert_equal {} [r raxlongest tree zzz]
        assert_equal 0 [r raxlongest tree zzz length]
    }

    test {RAXPREFIXES orders ancestors and applies MAXLEN before deepest COUNT} {
        r del tree
        foreach path {{} a ab abc abcd} {
            r raxset tree $path fields 1 f "value:$path"
        }
        assert_equal {0 1 2 3 4} [r raxprefixes tree abcde lengths]
        assert_equal {2 3} [r raxprefixes tree abcde lengths count 2 maxlen 3]
        assert_equal [list [list 2 [list value:ab]] [list 3 [list value:abc]]] \
            [r raxprefixes tree abcde lengths fields 1 f count 2 maxlen 3]
        assert_equal [list {}] [r raxprefixes tree zzz maxlen 0]
        assert_equal [list {}] [r raxprefixes tree zzz]
    }

    test {Prefix matching and subtree deletion are binary safe} {
        r del tree
        set p0 [binary format H* 00]
        set p1 [binary format H* 0061]
        set p2 [binary format H* 006100ff]
        set sibling [binary format H* 0062]
        set query [binary format H* 006100ff7a]
        foreach path [list {} $p0 $p1 $p2 $sibling] {
            r raxset tree $path fields 1 f "value:$path"
        }
        assert_equal {0 1 2 4} [r raxprefixes tree $query lengths]
        assert_equal 4 [r raxlongest tree $query length]
        assert_equal 2 [r raxdelprefix tree $p1]
        assert_equal [list {} $p0 $sibling] [lindex [r raxscan tree 0 count 100] 1]
    }

    test {RAXDEL removes fields, prunes empty payloads, and preserves descendants} {
        r del tree
        r raxset tree a fields 2 f1 v1 f2 v2
        r raxset tree ab fields 1 f child
        assert_equal 1 [r raxdel tree a missing f1 missing]
        assert_equal {{} v2} [r raxget tree a f1 f2]
        assert_equal 1 [r raxdel tree a f2]
        assert_equal 1 [r raxcard tree]
        assert_equal child [r raxget tree ab f]
        assert_equal 1 [r raxdel tree ab]
        assert_equal 1 [r exists tree]
        assert_equal radix [r type tree]
        assert_equal 0 [r raxcard tree]
        assert_equal 0 [r raxdel tree ab]
    }

    test {RAXDELPREFIX deletes only descendants and empty prefix clears the tree} {
        r del tree
        foreach path {a ab abc ac b ba} {r raxset tree $path fields 1 f $path}
        assert_equal 2 [r raxdelprefix tree ab]
        set result [r raxscan tree 0 count 100]
        assert_equal 0 [lindex $result 0]
        assert_equal {a ac b ba} [lindex $result 1]
        assert_equal 4 [r raxdelprefix tree {}]
        assert_equal 1 [r exists tree]
        assert_equal radix [r type tree]
        assert_equal 0 [r raxcard tree]
        assert_equal 0 [r raxdelprefix tree anything]
    }

    test {RAXDELPREFIX deletes matching paths in fixed-size chunks} {
        r del tree
        set assignments {}
        for {set i 0} {$i < 600} {incr i} {
            lappend assignments "delete:$i" fields 1 f "value:$i"
        }
        lappend assignments keep:a fields 1 f one keep:b fields 1 f two keep:c fields 1 f three
        assert_equal OK [r raxmset tree {*}$assignments]
        assert_equal 603 [r raxcard tree]
        assert_equal 600 [r raxdelprefix tree delete:]
        assert_equal 3 [r raxcard tree]
        assert_equal {keep:a keep:b keep:c} [lindex [r raxscan tree 0 count 100] 1]
    }

    test {Radix write commands account server dirty by logical mutations} {
        r del dirty-tree empty-dirty-tree
        r save

        assert_equal OK [r raxset dirty-tree p fields 2 f1 v1 f2 v2]
        assert_equal 2 [s rdb_changes_since_last_save]
        assert_equal {} [r raxset dirty-tree p fnx fields 2 f1 ignored missing ignored]
        assert_equal 2 [s rdb_changes_since_last_save]

        assert_equal OK [r raxmset dirty-tree p fields 1 f1 updated \
            branch:a fields 1 f one branch:b fields 1 f two]
        assert_equal 5 [s rdb_changes_since_last_save]
        assert_equal 2 [r raxdel dirty-tree p f1 f2]
        assert_equal 7 [s rdb_changes_since_last_save]
        assert_equal 2 [r raxdelprefix dirty-tree branch:]
        assert_equal 9 [s rdb_changes_since_last_save]
        assert_equal 0 [r raxdelprefix dirty-tree branch:]
        assert_equal 9 [s rdb_changes_since_last_save]

        assert_equal OK [r raxmset empty-dirty-tree a fields 1 f one \
            b fields 1 f two c fields 1 f three]
        r save
        assert_equal 3 [r raxdelprefix empty-dirty-tree {}]
        assert_equal 3 [s rdb_changes_since_last_save]
    }

    test {RAXSCAN uses an opaque cursor and traverses lexicographically} {
        r del tree
        foreach path {{} b aa a ab c} {r raxset tree $path fields 1 f "v:$path"}
        set cursor 0
        set paths {}
        while 1 {
            set page [r raxscan tree $cursor count 2]
            set cursor [lindex $page 0]
            foreach path [lindex $page 1] {lappend paths $path}
            if {$cursor eq "0"} break
        }
        assert_equal {{} a aa ab b c} $paths

        set prefixed [r raxscan tree 0 prefix a count 100 withvalues]
        assert_equal 0 [lindex $prefixed 0]
        assert_equal {a aa ab} [lmap entry [lindex $prefixed 1] {lindex $entry 0}]
        foreach entry [lindex $prefixed 1] {
            assert_equal "v:[lindex $entry 0]" [dict get [dict create {*}[lindex $entry 1]] f]
        }
    }

    test {RAXSCAN ends the traversal without an extra empty call} {
        r del tree
        r raxset tree a fields 1 f v
        r raxset tree b fields 1 f v
        set page [r raxscan tree 0 count 2]
        assert_equal 0 [lindex $page 0]
        assert_equal {a b} [lindex $page 1]

        set page [r raxscan tree 0 count 1]
        assert_equal {a} [lindex $page 1]
        assert {[lindex $page 0] ne "0"}
        set page [r raxscan tree [lindex $page 0] count 1]
        assert_equal {b} [lindex $page 1]
        assert_equal 0 [lindex $page 0]

        r raxset tree ba fields 1 f v
        set page [r raxscan tree 0 prefix b count 2]
        assert_equal {b ba} [lindex $page 1]
        assert_equal 0 [lindex $page 0]
    }

    test {Radix commands return WRONGTYPE consistently} {
        r set notradix value
        foreach command {
            {raxset notradix p fields 1 f v}
            {raxmset notradix p fields 1 f v}
            {raxget notradix p f}
            {raxmget notradix p fields 1 f}
            {raxgetall notradix p}
            {raxexists notradix p}
            {raxdel notradix p}
            {raxlongest notradix p}
            {raxprefixes notradix p}
            {raxdelprefix notradix p}
            {raxscan notradix 0}
            {raxcard notradix}
        } {
            assert_error WRONGTYPE* {r {*}$command}
        }
    }

    test {Radix option syntax rejects ambiguous and invalid inputs} {
        r del tree
        assert_error ERR*syntax* {r raxset tree p fnx fxx fields 1 f v}
        assert_error ERR*syntax* {r raxset tree p fnx fnx fields 1 f v}
        assert_error ERR*syntax* {r raxset tree p fields 2 f v}
        assert_error ERR*syntax* {r raxset tree p fields 1 f v extra}
        assert_error ERR*value*out*range* {r raxset tree p fields 0 f v}
        assert_error ERR*wrong*number* {r raxmset tree p f v}
        assert_error ERR*value*out*range* {r raxmset tree p fields 0 f v}
        assert_error ERR*syntax* {r raxmset tree p fields 2 f v}
        assert_error ERR*syntax* {r raxmset tree p fields 1 f v broken}
        assert_error ERR*wrong*number* {r raxmget tree p f}
        assert_error ERR*value*out*range* {r raxmget tree p fields 0 f}
        assert_error ERR*syntax* {r raxmget tree p fields 2 f}
        assert_error ERR*syntax* {r raxmget tree p fields 1 f broken}
        assert_equal 0 [r exists tree]
        assert_error ERR*syntax* {r raxlongest tree p withvalues fields 1 f}
        assert_error ERR*syntax* {r raxlongest tree p lengths}
        assert_error ERR*range* {r raxlongest tree p fields 0}
        assert_error ERR*syntax* {r raxprefixes tree p length}
        assert_error ERR*range* {r raxprefixes tree p fields 0}
        assert_error ERR*greater*zero* {r raxprefixes tree p count 0}
        assert_error ERR*non-negative* {r raxprefixes tree p maxlen -1}
        assert_error ERR*syntax* {r raxprefixes tree p fields 2 only-one}
        assert_error ERR*invalid*cursor* {r raxscan tree invalid}
        assert_error ERR*syntax* {r raxscan tree 0 count 1 count 2}
        assert_error ERR*value*out*range* {r raxscan tree 0 count 0}
    }

    test {Command metadata, ACL category, RESP3, and transactions expose the native type} {
        assert_equal radix [dict get [dict get [r command docs raxset] raxset] group]
        assert_equal 9.2.0 [dict get [dict get [r command docs raxset] raxset] since]
        assert {[lsearch -exact [r command list filterby aclcat radix] raxset] >= 0}
        assert {[lsearch -exact [r acl cat radix] raxprefixes] >= 0}
        assert {[lsearch -exact [r acl cat radix] raxmset] >= 0}
        assert {[lsearch -exact [r acl cat radix] raxexists] >= 0}
        r del tree
        r multi
        r raxset tree a fields 1 f one
        r raxset tree ab fields 1 f two
        assert_equal {OK OK} [r exec]
        set scan [r scan 0 type radix count 100]
        assert {[lsearch -exact [lindex $scan 1] tree] >= 0}
        r hello 3
        assert_equal {1 2} [r raxprefixes tree abc lengths]
        assert_equal two [r raxget tree ab f]
        r hello 2
    }

    test {RESP3 replies payloads as maps and field selections as arrays} {
        r del tree
        r raxset tree a fields 1 f1 v1
        r hello 3
        r readraw 1
        r deferred 1
        r raxgetall tree a
        assert_equal [r read] {%1}
        foreach _ {1 2 3 4} { r read }
        r raxgetall tree missing
        assert_equal [r read] {%0}
        r raxlongest tree a withvalues
        assert_equal [r read] {*2}
        assert_equal [r read] {$1}
        assert_equal [r read] {a}
        assert_equal [r read] {%1}
        foreach _ {1 2 3 4} { r read }
        r raxlongest tree a fields 1 f1
        assert_equal [r read] {*2}
        assert_equal [r read] {$1}
        assert_equal [r read] {a}
        assert_equal [r read] {*1}
        foreach _ {1 2} { r read }
        r readraw 0
        r deferred 0
        if {$::force_resp3} {
            r hello 3
        } else {
            r hello 2
        }
        assert_equal {f1 v1} [r raxgetall tree a]
    } {} {resp3}

    test {Radix keyspace notifications use the radix class} {
        r config set notify-keyspace-events Kr
        r del notify-tree
        set rd1 [valkey_deferring_client]
        with_cleanup {
            assert_equal {1} [psubscribe $rd1 *]
            assert_equal rK [lindex [r config get notify-keyspace-events] 1]
            r raxset notify-tree a fields 1 f one
            r raxmset notify-tree ab fields 1 f two
            r raxdel notify-tree a
            r raxdelprefix notify-tree a
            assert_match "pmessage * __keyspace@*__:notify-tree raxset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree raxmset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree raxdel" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree raxdelprefix" [$rd1 read]

            r config set notify-keyspace-events Krg
            assert_equal grK [lindex [r config get notify-keyspace-events] 1]
            r raxset notify-tree a fields 1 f one
            r raxdel notify-tree a
            assert_equal 1 [r exists notify-tree]
            r del notify-tree
            assert_match "pmessage * __keyspace@*__:notify-tree raxset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree raxdel" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree del" [$rd1 read]

            r raxset notify-tree a fields 1 f one
            r raxset notify-tree ab fields 1 f two
            r raxdelprefix notify-tree {}
            assert_equal 1 [r exists notify-tree]
            r del notify-tree
            assert_match "pmessage * __keyspace@*__:notify-tree raxset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree raxset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree raxdelprefix" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree del" [$rd1 read]
        } {
            catch {$rd1 close}
            r config set notify-keyspace-events {}
        }
    }

    test {UNLINK asynchronously frees a large radix object} {
        r del tree
        for {set i 0} {$i < 100} {incr i} {
            r raxset tree "path:$i" fields 1 field "value:$i"
        }
        assert_equal 100 [r raxcard tree]
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
        r raxset $tree {} fields 1 root value
        r raxset $tree abc fields 2 f1 v1 f2 v2
        r pexpire $tree 60000
        set digest_before [r debug digest]
        assert {[r memory usage $tree] > 0}
        assert_equal 1 [r copy $tree $tree_copy]
        assert {[r pttl $tree_copy] > 0}
        assert_equal {root value} [r raxgetall $tree_copy {}]
        assert_equal 2 [r raxcard $tree_copy]
        set dumped [r dump $tree]
        assert_equal OK [r restore $tree_restored 0 $dumped]
        assert_equal [r raxprefixes $tree abc withvalues] [r raxprefixes $tree_restored abc withvalues]
        assert {$digest_before ne ""}
    } {} {needs:debug}

    test {Empty Radix survives COPY, DUMP/RESTORE, and RDB reload} {
        r del empty-tree empty-copy empty-restored
        r raxset empty-tree path fields 1 field value
        assert_equal 1 [r raxdel empty-tree path]
        assert_equal 1 [r exists empty-tree]
        assert_equal radix [r type empty-tree]
        assert_equal 0 [r raxcard empty-tree]

        assert_equal 1 [r copy empty-tree empty-copy]
        assert_equal radix [r type empty-copy]
        assert_equal 0 [r raxcard empty-copy]

        set dumped [r dump empty-tree]
        assert_equal OK [r restore empty-restored 0 $dumped]
        assert_equal radix [r type empty-restored]
        assert_equal 0 [r raxcard empty-restored]

        r debug reload
        foreach key {empty-tree empty-copy empty-restored} {
            assert_equal 1 [r exists $key]
            assert_equal radix [r type $key]
            assert_equal 0 [r raxcard $key]
            assert_equal {0 {}} [r raxscan $key 0]
        }
        assert_equal 1 [r del empty-tree]
        assert_equal 1 [r unlink empty-copy]
        assert_equal 0 [r exists empty-tree]
        assert_equal 0 [r exists empty-copy]
        assert_equal 1 [r exists empty-restored]
    } {} {needs:debug}

    test {RDB reload restores radix paths, binary data, root payload, and TTL} {
        r del tree
        set binary_path [binary format H* 000102ff]
        set binary_value [binary format H* 7600616cff]
        r raxset tree {} fields 1 root root-value
        r raxset tree $binary_path fields 1 field $binary_value
        r pexpire tree 60000
        r debug reload
        assert_equal radix [r type tree]
        assert_equal root-value [r raxget tree {} root]
        assert_equal $binary_value [r raxget tree $binary_path field]
        assert_equal 2 [r raxcard tree]
        assert {[r pttl tree] > 0}
    } {} {needs:debug}

    test {RESTORE rejects malformed radix payloads without destabilizing the server} {
        r debug set-skip-checksum-validation 1

        r del source
        r del corrupt
        r raxset source p fields 1 f v
        set empty_payload [string replace [r dump source] 4 4 "\x00"]
        catch {r restore corrupt 0 $empty_payload} err
        assert_match {*Bad data format*} $err

        r del source
        r del corrupt
        r raxset source a fields 1 f x
        r raxset source b fields 1 f y
        set duplicate_path [r dump source]
        set duplicate_path [string replace $duplicate_path 10 10 [string index $duplicate_path 3]]
        catch {r restore corrupt 0 $duplicate_path} err
        assert_match {*Bad data format*} $err

        r del source
        r del corrupt
        r raxset source p fields 1 a x
        r raxset source p fields 1 b y
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
            r raxset tree path fields 1 "field:$i" "value:$i"
        }
        assert_equal 1 [r raxcard tree]
        assert_equal value:599 [r raxget tree path field:599]
        r debug reload
        assert_equal value:0 [r raxget tree path field:0]
        assert_equal value:599 [r raxget tree path field:599]
        assert_equal 1200 [llength [r raxgetall tree path]]
    } {} {needs:debug}

    test {valkey-check-rdb validates and reports the native radix type} {
        r del tree
        r raxset tree {} fields 1 root value
        r raxset tree abc fields 1 f v
        r save
        set dir [lindex [r config get dir] 1]
        set filename [lindex [r config get dbfilename] 1]
        set output [exec $::VALKEY_CHECK_RDB_BIN [file join $dir $filename] --stats --format info]
        assert_match {*RDB looks OK*} $output
        assert_match {*radix*} $output
    } {} {external:skip}
}

start_server {tags {radix needs:debug} overrides {appendonly yes aof-use-rdb-preamble no}} {
    test {AOF rewrite and reload preserve populated and empty Radix values} {
        r raxset tree {} fields 1 root value
        r raxset tree abc fields 2 f1 v1 f2 v2
        r raxset tree abcd fields 1 child v3
        r raxdel tree abc f1
        r raxset empty-tree path fields 1 field value
        r raxdel empty-tree path
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
        assert_equal 3 [r raxcard tree]
        assert_equal value [r raxget tree {} root]
        assert_equal {{} v2} [r raxget tree abc f1 f2]
        assert_equal v3 [r raxget tree abcd child]
        assert_equal 1 [r exists empty-tree]
        assert_equal radix [r type empty-tree]
        assert_equal 0 [r raxcard empty-tree]
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

            $primary raxset tree a fields 1 f one
            $primary raxmset tree ab fields 1 f two abc fields 1 f three
            assert_equal {} [$primary raxset tree a fnx fields 1 f ignored]
            $primary raxdelprefix tree ab
            $primary raxset empty-tree path fields 1 field value
            $primary raxdel empty-tree path
            wait_for_ofs_sync $primary $replica
            assert_equal 1 [$replica raxcard tree]
            assert_equal one [$replica raxget tree a f]
            assert_equal {} [$replica raxget tree ab f]
            assert_equal 1 [$replica exists empty-tree]
            assert_equal radix [$replica type empty-tree]
            assert_equal 0 [$replica raxcard empty-tree]
        }
    }
}
