start_server {tags {radix}} {
    test {RSET creates a native radix object and exact reads are binary safe} {
        set path [binary format H* 0001ff]
        set field [binary format H* 660069656c64]
        set value [binary format H* 7600616c7565ff]
        assert_equal OK [r rset tree $path $field $value]
        assert_equal radix [r type tree]
        assert_equal radix [r object encoding tree]
        assert_equal 1 [r rcard tree]
        assert_equal $value [r rget tree $path $field]
        assert_equal {} [r rget tree $path missing]
        assert_equal [list $value {}] [r rmget tree $path $field missing]
        assert_equal 0 [r rcard missing]
    }

    test {RSET NX and XX apply to a field rather than only the path} {
        r del tree
        assert_equal {} [r rset tree path f v xx]
        assert_equal 0 [r exists tree]
        assert_equal OK [r rset tree path f v nx]
        assert_equal {} [r rset tree path f replacement nx]
        assert_equal v [r rget tree path f]
        assert_equal {} [r rset tree path missing v xx]
        assert_equal OK [r rset tree path f replacement xx]
        assert_equal replacement [r rget tree path f]
        assert_equal OK [r rset tree path second two nx]
        assert_equal 1 [r rcard tree]
    }

    test {Exact path operations do not confuse ancestors and descendants} {
        r del tree
        r rset tree a f one
        r rset tree ab f two
        r rset tree abc f three
        assert_equal one [r rget tree a f]
        assert_equal two [r rget tree ab f]
        assert_equal three [r rget tree abc f]
        assert_equal {} [r rget tree abcd f]
        assert_equal 3 [r rcard tree]
    }

    test {RGETALL and RMGET preserve field semantics} {
        r del tree
        r rset tree path f1 v1
        r rset tree path f2 v2
        set values [dict create {*}[r rgetall tree path]]
        assert_equal v1 [dict get $values f1]
        assert_equal v2 [dict get $values f2]
        assert_equal {v2 {} v1} [r rmget tree path f2 missing f1]
        assert_equal {} [r rgetall tree missing]
    }

    test {RLONGEST handles root, compressed edges, lengths, values, and field filters} {
        r del tree
        r rset tree {} root root-value
        r rset tree a f v-a
        r rset tree abc f1 v1
        r rset tree abc f2 v2
        r rset tree abcdef f deep
        assert_equal abc [r rlongest tree abczzz]
        assert_equal 3 [r rlongest tree abczzz length]
        assert_equal [list abc [list v1 {} v2]] [r rlongest tree abczzz fields 3 f1 missing f2]
        set withvalues [r rlongest tree abczzz withvalues]
        assert_equal abc [lindex $withvalues 0]
        set payload [dict create {*}[lindex $withvalues 1]]
        assert_equal v1 [dict get $payload f1]
        assert_equal v2 [dict get $payload f2]
        assert_equal {} [r rlongest tree zzz]
        assert_equal 0 [r rlongest tree zzz length]
        assert_equal {} [r rlongest missing anything]
    }

    test {RLONGEST replies null when no stored path prefixes the query} {
        r del tree
        r rset tree abc f v
        set nullres {$-1}
        if {$::force_resp3} {
            set nullres {_}
        }
        r readraw 1
        r deferred 1
        r rlongest tree zzz
        assert_equal [r read] $nullres
        r rlongest tree zzz length
        assert_equal [r read] $nullres
        r readraw 0
        r deferred 0
        # A stored empty root path is a real match of length 0, which the client
        # renders exactly like the null reply above.
        r rset tree {} root v
        assert_equal {} [r rlongest tree zzz]
        assert_equal 0 [r rlongest tree zzz length]
    }

    test {RPREFIXES orders ancestors and applies MAXLEN before deepest COUNT} {
        r del tree
        foreach path {{} a ab abc abcd} {
            r rset tree $path f "value:$path"
        }
        assert_equal {0 1 2 3 4} [r rprefixes tree abcde lengths]
        assert_equal {2 3} [r rprefixes tree abcde lengths count 2 maxlen 3]
        assert_equal [list [list 2 [list value:ab]] [list 3 [list value:abc]]] \
            [r rprefixes tree abcde lengths fields 1 f count 2 maxlen 3]
        assert_equal [list {}] [r rprefixes tree zzz maxlen 0]
        assert_equal [list {}] [r rprefixes tree zzz]
    }

    test {Prefix matching and subtree deletion are binary safe} {
        r del tree
        set p0 [binary format H* 00]
        set p1 [binary format H* 0061]
        set p2 [binary format H* 006100ff]
        set sibling [binary format H* 0062]
        set query [binary format H* 006100ff7a]
        foreach path [list {} $p0 $p1 $p2 $sibling] {
            r rset tree $path f "value:$path"
        }
        assert_equal {0 1 2 4} [r rprefixes tree $query lengths]
        assert_equal 4 [r rlongest tree $query length]
        assert_equal 2 [r rdelprefix tree $p1]
        assert_equal [list {} $p0 $sibling] [lindex [r rscan tree 0 count 100] 1]
    }

    test {RDEL removes fields, prunes empty payloads, and preserves descendants} {
        r del tree
        r rset tree a f1 v1
        r rset tree a f2 v2
        r rset tree ab f child
        assert_equal 1 [r rdel tree a missing f1 missing]
        assert_equal {{} v2} [r rmget tree a f1 f2]
        assert_equal 1 [r rdel tree a f2]
        assert_equal 1 [r rcard tree]
        assert_equal child [r rget tree ab f]
        assert_equal 1 [r rdel tree ab]
        assert_equal 0 [r exists tree]
        assert_equal 0 [r rdel tree ab]
    }

    test {RDELPREFIX deletes only descendants and empty prefix clears the key} {
        r del tree
        foreach path {a ab abc ac b ba} {r rset tree $path f $path}
        assert_equal 2 [r rdelprefix tree ab]
        set result [r rscan tree 0 count 100]
        assert_equal 0 [lindex $result 0]
        assert_equal {a ac b ba} [lindex $result 1]
        assert_equal 4 [r rdelprefix tree {}]
        assert_equal 0 [r exists tree]
        assert_equal 0 [r rdelprefix tree anything]
    }

    test {RSCAN uses an opaque cursor and traverses lexicographically} {
        r del tree
        foreach path {{} b aa a ab c} {r rset tree $path f "v:$path"}
        set cursor 0
        set paths {}
        while 1 {
            set page [r rscan tree $cursor count 2]
            set cursor [lindex $page 0]
            foreach path [lindex $page 1] {lappend paths $path}
            if {$cursor eq "0"} break
        }
        assert_equal {{} a aa ab b c} $paths

        set prefixed [r rscan tree 0 prefix a count 100 withvalues]
        assert_equal 0 [lindex $prefixed 0]
        assert_equal {a aa ab} [lmap entry [lindex $prefixed 1] {lindex $entry 0}]
        foreach entry [lindex $prefixed 1] {
            assert_equal "v:[lindex $entry 0]" [dict get [dict create {*}[lindex $entry 1]] f]
        }
    }

    test {RSCAN ends the traversal without an extra empty call} {
        r del tree
        r rset tree a f v
        r rset tree b f v
        set page [r rscan tree 0 count 2]
        assert_equal 0 [lindex $page 0]
        assert_equal {a b} [lindex $page 1]

        set page [r rscan tree 0 count 1]
        assert_equal {a} [lindex $page 1]
        assert {[lindex $page 0] ne "0"}
        set page [r rscan tree [lindex $page 0] count 1]
        assert_equal {b} [lindex $page 1]
        assert_equal 0 [lindex $page 0]

        r rset tree ba f v
        set page [r rscan tree 0 prefix b count 2]
        assert_equal {b ba} [lindex $page 1]
        assert_equal 0 [lindex $page 0]
    }

    test {Radix commands return WRONGTYPE consistently} {
        r set notradix value
        foreach command {
            {rset notradix p f v}
            {rget notradix p f}
            {rmget notradix p f}
            {rgetall notradix p}
            {rdel notradix p}
            {rlongest notradix p}
            {rprefixes notradix p}
            {rdelprefix notradix p}
            {rscan notradix 0}
            {rcard notradix}
        } {
            assert_error WRONGTYPE* {r {*}$command}
        }
    }

    test {Radix option syntax rejects ambiguous and invalid inputs} {
        r del tree
        assert_error ERR*syntax* {r rset tree p f v nx xx}
        assert_error ERR*syntax* {r rset tree p f v nx nx}
        assert_error ERR*syntax* {r rlongest tree p withvalues fields 1 f}
        assert_error ERR*syntax* {r rlongest tree p lengths}
        assert_error ERR*range* {r rlongest tree p fields 0}
        assert_error ERR*syntax* {r rprefixes tree p length}
        assert_error ERR*range* {r rprefixes tree p fields 0}
        assert_error ERR*greater*zero* {r rprefixes tree p count 0}
        assert_error ERR*non-negative* {r rprefixes tree p maxlen -1}
        assert_error ERR*syntax* {r rprefixes tree p fields 2 only-one}
        assert_error ERR*invalid*cursor* {r rscan tree invalid}
        assert_error ERR*syntax* {r rscan tree 0 count 1 count 2}
        assert_error ERR*value*out*range* {r rscan tree 0 count 0}
    }

    test {Command metadata, ACL category, RESP3, and transactions expose the native type} {
        assert_equal radix [dict get [dict get [r command docs rset] rset] group]
        assert_equal 9.2.0 [dict get [dict get [r command docs rset] rset] since]
        assert {[lsearch -exact [r command list filterby aclcat radix] rset] >= 0}
        assert {[lsearch -exact [r acl cat radix] rprefixes] >= 0}
        r del tree
        r multi
        r rset tree a f one
        r rset tree ab f two
        assert_equal {OK OK} [r exec]
        set scan [r scan 0 type radix count 100]
        assert {[lsearch -exact [lindex $scan 1] tree] >= 0}
        r hello 3
        assert_equal {1 2} [r rprefixes tree abc lengths]
        assert_equal two [r rget tree ab f]
        r hello 2
    }

    test {RESP3 replies payloads as maps and field selections as arrays} {
        r del tree
        r rset tree a f1 v1
        r hello 3
        r readraw 1
        r deferred 1
        r rgetall tree a
        assert_equal [r read] {%1}
        foreach _ {1 2 3 4} { r read }
        r rgetall tree missing
        assert_equal [r read] {%0}
        r rlongest tree a withvalues
        assert_equal [r read] {*2}
        assert_equal [r read] {$1}
        assert_equal [r read] {a}
        assert_equal [r read] {%1}
        foreach _ {1 2 3 4} { r read }
        r rlongest tree a fields 1 f1
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
        assert_equal {f1 v1} [r rgetall tree a]
    } {} {resp3}

    test {Radix keyspace notifications use the radix class} {
        r config set notify-keyspace-events Kr
        r del notify-tree
        set rd1 [valkey_deferring_client]
        with_cleanup {
            assert_equal {1} [psubscribe $rd1 *]
            assert_equal rK [lindex [r config get notify-keyspace-events] 1]
            r rset notify-tree a f one
            r rset notify-tree ab f two
            r rdel notify-tree a
            r rdelprefix notify-tree a
            assert_match "pmessage * __keyspace@*__:notify-tree rset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree rset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree rdel" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree rdelprefix" [$rd1 read]

            r config set notify-keyspace-events Krg
            assert_equal grK [lindex [r config get notify-keyspace-events] 1]
            r rset notify-tree a f one
            r rdel notify-tree a
            assert_match "pmessage * __keyspace@*__:notify-tree rset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree rdel" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree del" [$rd1 read]

            r rset notify-tree a f one
            r rset notify-tree ab f two
            r rdelprefix notify-tree {}
            assert_match "pmessage * __keyspace@*__:notify-tree rset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree rset" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree rdelprefix" [$rd1 read]
            assert_match "pmessage * __keyspace@*__:notify-tree del" [$rd1 read]
        } {
            catch {$rd1 close}
            r config set notify-keyspace-events {}
        }
    }

    test {UNLINK asynchronously frees a large radix object} {
        r del tree
        for {set i 0} {$i < 100} {incr i} {
            r rset tree "path:$i" field "value:$i"
        }
        assert_equal 100 [r rcard tree]
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
        r rset $tree {} root value
        r rset $tree abc f1 v1
        r rset $tree abc f2 v2
        r pexpire $tree 60000
        set digest_before [r debug digest]
        assert {[r memory usage $tree] > 0}
        assert_equal 1 [r copy $tree $tree_copy]
        assert {[r pttl $tree_copy] > 0}
        assert_equal {root value} [r rgetall $tree_copy {}]
        assert_equal 2 [r rcard $tree_copy]
        set dumped [r dump $tree]
        assert_equal OK [r restore $tree_restored 0 $dumped]
        assert_equal [r rprefixes $tree abc withvalues] [r rprefixes $tree_restored abc withvalues]
        assert {$digest_before ne ""}
    } {} {needs:debug}

    test {RDB reload restores radix paths, binary data, root payload, and TTL} {
        r del tree
        set binary_path [binary format H* 000102ff]
        set binary_value [binary format H* 7600616cff]
        r rset tree {} root root-value
        r rset tree $binary_path field $binary_value
        r pexpire tree 60000
        r debug reload
        assert_equal radix [r type tree]
        assert_equal root-value [r rget tree {} root]
        assert_equal $binary_value [r rget tree $binary_path field]
        assert_equal 2 [r rcard tree]
        assert {[r pttl tree] > 0}
    } {} {needs:debug}

    test {RESTORE rejects malformed radix payloads without destabilizing the server} {
        r debug set-skip-checksum-validation 1

        r del source
        r del corrupt
        r rset source p f v
        set empty_payload [string replace [r dump source] 4 4 "\x00"]
        catch {r restore corrupt 0 $empty_payload} err
        assert_match {*Bad data format*} $err

        r del source
        r del corrupt
        r rset source a f x
        r rset source b f y
        set duplicate_path [r dump source]
        set duplicate_path [string replace $duplicate_path 10 10 [string index $duplicate_path 3]]
        catch {r restore corrupt 0 $duplicate_path} err
        assert_match {*Bad data format*} $err

        r del source
        r del corrupt
        r rset source p a x
        r rset source p b y
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
            r rset tree path "field:$i" "value:$i"
        }
        assert_equal 1 [r rcard tree]
        assert_equal value:599 [r rget tree path field:599]
        r debug reload
        assert_equal value:0 [r rget tree path field:0]
        assert_equal value:599 [r rget tree path field:599]
        assert_equal 1200 [llength [r rgetall tree path]]
    } {} {needs:debug}

    test {valkey-check-rdb validates and reports the native radix type} {
        r del tree
        r rset tree {} root value
        r rset tree abc f v
        r save
        set dir [lindex [r config get dir] 1]
        set filename [lindex [r config get dbfilename] 1]
        set output [exec $::VALKEY_CHECK_RDB_BIN [file join $dir $filename] --stats --format info]
        assert_match {*RDB looks OK*} $output
        assert_match {*radix*} $output
    } {} {external:skip}
}

start_server {tags {radix needs:debug} overrides {appendonly yes aof-use-rdb-preamble no}} {
    test {AOF rewrite and reload preserve radix values} {
        r rset tree {} root value
        r rset tree abc f1 v1
        r rset tree abc f2 v2
        r rset tree abcd child v3
        r rdel tree abc f1
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
        assert_equal 3 [r rcard tree]
        assert_equal value [r rget tree {} root]
        assert_equal {{} v2} [r rmget tree abc f1 f2]
        assert_equal v3 [r rget tree abcd child]
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

            $primary rset tree a f one
            $primary rset tree ab f two
            $primary rset tree abc f three
            assert_equal {} [$primary rset tree a f ignored nx]
            $primary rdelprefix tree ab
            wait_for_ofs_sync $primary $replica
            assert_equal 1 [$replica rcard tree]
            assert_equal one [$replica rget tree a f]
            assert_equal {} [$replica rget tree ab f]
        }
    }
}
