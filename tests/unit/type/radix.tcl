start_server {tags {"radix"}} {
    proc radix_payload_dict {reply} {
        return [dict create {*}$reply]
    }

    proc radix_scan_all {key args} {
        set cursor 0
        set paths {}
        while {1} {
            set reply [r rscan $key $cursor {*}$args]
            set cursor [lindex $reply 0]
            lappend paths {*}[lindex $reply 1]
            if {$cursor eq "0"} break
        }
        return $paths
    }

    test {RSET supports binary-safe and empty path, field and value} {
        r del rt
        set binary_path "a\x00b"
        set binary_field "f\x00x"
        set binary_value "v\x00z"
        assert_equal OK [r rset rt {} {} {}]
        assert_equal OK [r rset rt $binary_path $binary_field $binary_value]
        assert_equal {} [r rget rt {} {}]
        assert_equal $binary_value [r rget rt $binary_path $binary_field]
        assert_equal 2 [r rcard rt]
    }

    test {RSET NX and XX conditions} {
        r del rt
        assert_equal OK [r rset rt path field one nx]
        assert_equal {} [r rset rt path field two nx]
        assert_equal one [r rget rt path field]
        assert_equal {} [r rset rt path missing two xx]
        assert_equal {} [r rset rt missing field two xx]
        assert_equal OK [r rset rt path field two xx]
        assert_equal two [r rget rt path field]
    }

    test {RGET and RMGET exact and missing reads preserve alignment} {
        r del rt
        r rset rt path a 1
        r rset rt path c {}
        assert_equal 1 [r rget rt path a]
        assert_equal {} [r rget rt path missing]
        assert_equal {} [r rget rt missing a]
        assert_equal {1 {} {} {}} [r rmget rt path a b c d]
        assert_equal {{} {}} [r rmget missing path a b]
    }

    test {RGETALL returns all field/value pairs and missing paths are empty} {
        r del rt
        r rset rt path a 1
        r rset rt path b 2
        assert_equal [dict create a 1 b 2] [radix_payload_dict [r rgetall rt path]]
        assert_equal {} [r rgetall rt missing]
        assert_equal {} [r rgetall missing path]
    }

    test {RDEL fields, whole paths, descendants and final key deletion} {
        r del rt
        r rset rt ab a 1
        r rset rt ab b 2
        r rset rt abcd child 3
        assert_equal 1 [r rdel rt ab a missing]
        # The parent still exists because field b remains, and so does its descendant.
        assert_equal 2 [r rcard rt]
        assert_equal 1 [r rdel rt ab]
        assert_equal {} [r rget rt ab b]
        assert_equal 3 [r rget rt abcd child]
        assert_equal 1 [r rdel rt abcd child]
        assert_equal none [r type rt]
        assert_equal 0 [r rcard rt]
    }

    test {RLONGEST default, LENGTH, WITHVALUES and FIELDS shapes} {
        r del rt
        r rset rt {} root R
        r rset rt ab a 1
        r rset rt ab b 2
        r rset rt abcd a 3
        assert_equal abcd [r rlongest rt abcde]
        assert_equal 4 [r rlongest rt abcde length]
        set reply [r rlongest rt abcde withvalues]
        assert_equal abcd [lindex $reply 0]
        assert_equal [dict create a 3] [radix_payload_dict [lindex $reply 1]]
        assert_equal {4 {3 {}}} [r rlongest rt abcde length fields 2 a missing]
        assert_equal {{} {R {}}} [r rlongest rt z fields 2 root missing]
        assert_equal {} [r rlongest missing query]
    }

    test {RPREFIXES ordering, LENGTHS, values, filtering, MAXLEN and deepest COUNT} {
        r del rt
        r rset rt {} root r
        r rset rt a f a
        r rset rt ab f ab
        r rset rt abcd f abcd
        r rset rt abcdef f abcdef
        assert_equal {{} a ab abcd abcdef} [r rprefixes rt abcdefg]
        assert_equal {0 1 2 4 6} [r rprefixes rt abcdefg lengths]
        assert_equal {2 4} [r rprefixes rt abcdefg lengths maxlen 4 count 2]
        assert_equal {} [r rprefixes rt abcdefg count 0]
        assert_equal {0 1 2 4 6} [r rprefixes rt abcdefg lengths count 9223372036854775807]
        assert_equal {{1 {a {}}} {2 {ab {}}}} \
            [r rprefixes rt abc lengthS fields 2 f missing maxlen 2 count 2]

        set values [r rprefixes rt abc withvalues count 2]
        assert_equal a [lindex [lindex $values 0] 0]
        assert_equal [dict create f a] [radix_payload_dict [lindex [lindex $values 0] 1]]
        assert_equal ab [lindex [lindex $values 1] 0]
        assert_equal [dict create f ab] [radix_payload_dict [lindex [lindex $values 1] 1]]
        assert_equal {} [r rprefixes missing abc]
    }

    test {RDELPREFIX deletes descendants and empty prefix clears the tree} {
        r del rt
        foreach path {{} a ab abc b ba} {
            r rset rt $path f $path
        }
        assert_equal 3 [r rdelprefix rt a]
        assert_equal {{} b ba} [r rprefixes rt baz]
        assert_equal 3 [r rdelprefix rt {}]
        assert_equal none [r type rt]
        assert_equal 0 [r rdelprefix rt anything]
    }

    test {RSCAN lexicographic traversal, PREFIX, COUNT and WITHVALUES} {
        r del rt
        foreach path {{} a aa ab b ba} {
            r rset rt $path f "v:$path"
        }
        assert_equal {{} a aa ab b ba} [radix_scan_all rt count 1]
        assert_equal {a aa ab} [radix_scan_all rt prefix a count 2]

        set cursor 0
        set found {}
        while {1} {
            set reply [r rscan rt $cursor prefix a count 1 withvalues]
            set cursor [lindex $reply 0]
            foreach entry [lindex $reply 1] {
                set path [lindex $entry 0]
                assert_equal [dict create f "v:$path"] [radix_payload_dict [lindex $entry 1]]
                lappend found $path
            }
            if {$cursor eq "0"} break
        }
        assert_equal {a aa ab} $found
    }

    test {RSCAN cursor is binary-safe and seeks after concurrent mutation} {
        r del rt
        foreach path {a c e} {r rset rt $path f $path}
        set first [r rscan rt 0 count 1]
        set cursor [lindex $first 0]
        assert_not_equal 0 $cursor
        assert_equal a [lindex [lindex $first 1] 0]
        r rdel rt a
        r rset rt b f b
        set rest {}
        while {$cursor ne "0"} {
            set reply [r rscan rt $cursor count 1]
            set cursor [lindex $reply 0]
            lappend rest {*}[lindex $reply 1]
        }
        assert_equal {b c e} $rest
    }

    test {RSCAN rejects malformed and mismatched cursors} {
        r del rt
        r rset rt a f v
        assert_error {*invalid radix cursor*} {r rscan rt malformed}
        set cursor [lindex [r rscan rt 0 prefix a count 1] 0]
        # A completed one-item scan returns 0, so add another path to obtain an opaque cursor.
        r rset rt ab f v
        set cursor [lindex [r rscan rt 0 prefix a count 1] 0]
        assert_error {*invalid radix cursor*} {r rscan rt $cursor prefix b count 1}
    }

    test {RCARD, TYPE, OBJECT ENCODING and MEMORY USAGE} {
        r del rt
        assert_equal 0 [r rcard rt]
        r rset rt a f 1
        r rset rt b f 2
        assert_equal 2 [r rcard rt]
        assert_equal radix [r type rt]
        assert_equal radix [r object encoding rt]
        assert {[r memory usage rt samples 10] > 0}
    }

    test {Radix commands return WRONGTYPE} {
        r set wrong value
        foreach command {
            {rset wrong p f v}
            {rget wrong p f}
            {rmget wrong p f}
            {rgetall wrong p}
            {rdel wrong p}
            {rlongest wrong p}
            {rprefixes wrong p}
            {rdelprefix wrong p}
            {rscan wrong 0}
            {rcard wrong}
        } {
            assert_error {WRONGTYPE*} [list r {*}$command]
        }
    }

    test {Radix commands enforce syntax and numeric ranges} {
        r del rt
        assert_error {*syntax*} {r rset rt p f v bad}
        assert_error {*wrong number*} {r rset rt p f}
        assert_error {*syntax*} {r rlongest rt q withvalues fields 1 f}
        assert_error {*syntax*} {r rlongest rt q fields -1}
        assert_error {*syntax*} {r rlongest rt q fields 2 one}
        assert_error {*syntax*} {r rprefixes rt q withvalues fields 1 f}
        assert_error {*COUNT must be non-negative*} {r rprefixes rt q count -1}
        assert_error {*MAXLEN must be non-negative*} {r rprefixes rt q maxlen -1}
        assert_error {*value is not an integer*} {r rprefixes rt q count nope}
        assert_error {*COUNT must be greater than 0*} {r rscan rt 0 count 0}
        assert_error {*COUNT must be greater than 0*} {r rscan rt 0 count -1}
        assert_error {*syntax*} {r rscan rt 0 prefix}
        assert_error {*syntax*} {r rscan rt 0 withvalues withvalues}
    }

    test {Radix keyspace notifications use the radix event class} {
        r config set notify-keyspace-events Kr
        r del notify-rt
        set rd [valkey_deferring_client]
        assert_equal 1 [psubscribe $rd __keyspace@*__:notify-rt]
        r rset notify-rt path field value
        assert_match {pmessage * * rset} [$rd read]
        r rdel notify-rt path
        assert_match {pmessage * * rdel} [$rd read]
        $rd close
        r config set notify-keyspace-events ""
    }

    test {Radix response shapes are stable under RESP2 and RESP3} {
        r del rt
        r rset rt ab a 1
        r rset rt ab b 2
        set resp2 [list \
            [r rmget rt ab a missing] \
            [r rlongest rt abc length fields 2 a missing] \
            [r rprefixes rt abc lengths fields 2 a missing]]
        r hello 3
        set resp3 [list \
            [r rmget rt ab a missing] \
            [r rlongest rt abc length fields 2 a missing] \
            [r rprefixes rt abc lengths fields 2 a missing]]
        r hello 2
        assert_equal $resp2 $resp3
    }
}
