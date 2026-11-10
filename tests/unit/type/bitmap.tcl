# Tests of the bitmap data type, stored as a Roaring bitmap. The bit commands
# must return the same results for a bitmap as for the string holding the same
# bytes, so most tests run the same commands on a string key and on a bitmap
# key and compare.

# Replaces the value at each key with an empty bitmap.
proc bitmap_new {args} {
    foreach key $args {
        r del $key
        assert_equal 1 [r bitconvert $key bitmap]
    }
}

# Deletes the string key 'plain' and makes 'roar' an empty bitmap, for
# bitmap_twin.
proc bitmap_twin_reset {} {
    r del plain
    bitmap_new roar
}

# Runs a command on the string key and on the bitmap key (names are given with
# the placeholder KEY), and checks that the replies are the same.
proc bitmap_twin {args} {
    set plain [catch {r {*}[string map {KEY plain} $args]} pres]
    set roar [catch {r {*}[string map {KEY roar} $args]} rres]
    assert_equal [list $plain $pres] [list $roar $rres]
    return $rres
}

# Returns the bytes of the bitmap at 'key'. It works on a copy, made with
# DUMP and RESTORE rather than COPY so that the key may be in any slot.
proc bitmap_bytes {key} {
    r del bitmap_bytes_tmp
    r restore bitmap_bytes_tmp 0 [r dump $key]
    r bitconvert bitmap_bytes_tmp string
    set bytes [r get bitmap_bytes_tmp]
    r del bitmap_bytes_tmp
    return $bytes
}

# Deletes each key with its own DEL, so that the keys may be in any slot.
proc bitmap_del {args} {
    foreach key $args { r del $key }
}

proc bitmap_assert_same_value {} {
    assert_equal bitmap [r type roar]
    assert_equal [r get plain] [bitmap_bytes roar]
}

# The portable format: a cookie (12346, no run containers), the number of
# containers, then for each its key and cardinality minus one, then for each
# its offset, then the containers (here, sorted arrays of 16-bit values).
# Everything is little endian.
proc roaring_array_payload {key values} {
    set n [llength $values]
    return [binary format iissi 12346 1 $key [expr {$n - 1}] 16][binary format s* $values]
}

start_server {tags {"bitmap"}} {
    test {Bitmap: bit writes keep the type of their key} {
        bitmap_del s b m
        r set s "foobar"
        bitmap_new b
        foreach k {s b m} {
            r setbit $k 7 1
            r bitfield $k set u8 100 255
        }
        assert_equal {string bitmap string} [list [r type s] [r type b] [r type m]]
        assert_encoding roaring b
        assert_equal "goobar" [string range [r get s] 0 5]
        assert_equal [r get m] [bitmap_bytes b]
        # Clearing every bit keeps the length, like a string.
        r bitfield b set u8 100 0
        r setbit b 7 0
        assert_equal 0 [r bitcount b]
        assert_equal [string repeat "\x00" 14] [bitmap_bytes b]
    }

    test {Bitmap: SETBIT on an integer-encoded string keeps a string} {
        r set k 12345
        assert_encoding int k
        r setbit k 100 0
        assert_equal string [r type k]
        assert_encoding raw k
        assert_equal "12345[string repeat "\x00" 8]" [r get k]
        # Shared integers too.
        r set k 1
        r setbit k 0 1
        assert_equal string [r type k]
        assert_equal "\xb1" [r get k]
    }

    test {Bitmap: SETBIT and GETBIT on a bitmap} {
        bitmap_new k
        assert_equal 0 [r setbit k 100 1]
        assert_equal 1 [r getbit k 100]
        assert_equal 0 [r getbit k 101]
        assert_equal 0 [r getbit k 1000000]
        assert_equal 1 [r setbit k 100 0]
        assert_equal 0 [r getbit k 100]
        assert_equal [string repeat "\x00" 13] [bitmap_bytes k]
    }

    test {Bitmap: string commands reply WRONGTYPE} {
        bitmap_new k
        r setbit k 10 1
        foreach cmd {{get k} {getdel k} {getex k} {getset k x} {strlen k} {getrange k 0 -1}
                     {append k x} {setrange k 0 x} {incr k} {decrby k 2} {incrbyfloat k 1.5}
                     {pfadd k a} {pfcount k} {pfmerge k}} {
            assert_error {*WRONGTYPE*} {r {*}$cmd}
        }
        assert_error {*must contain string values*} {r lcs k k}
        assert_equal bitmap [r type k]
        # SET replaces the bitmap, like any value.
        r set k v
        assert_equal string [r type k]
    }

    test {Bitmap: bit commands reply WRONGTYPE for other types} {
        r del list
        r rpush list a
        foreach cmd {{setbit list 0 1} {getbit list 0} {bitcount list} {bitpos list 1}
                     {bitfield list get u8 0} {bitfield_ro list get u8 0} {bitop or list list}
                     {bitconvert list bitmap} {r.export list}} {
            assert_error {*WRONGTYPE*} {r {*}$cmd}
        }
        r del list
    }

    test {Bitmap: TYPE, SCAN TYPE and OBJECT ENCODING} {
        r flushdb
        bitmap_new b1 b2
        r setbit b1 1 1
        r set s v
        assert_equal {b1 b2} [lsort [lindex [r scan 0 type bitmap count 100] 1]]
        assert_equal {s} [lindex [r scan 0 type string count 100] 1]
        assert_equal roaring [r object encoding b1]
    }

    test {Bitmap: BITCONVERT} {
        r del k
        assert_equal 0 [r bitconvert k string]
        assert_equal 0 [r exists k]
        r set k "foobar"
        r expire k 1000
        assert_equal 1 [r bitconvert k bitmap]
        assert_equal bitmap [r type k]
        assert_range [r ttl k] 900 1000
        assert_equal 0 [r bitconvert k BITMAP]
        assert_equal 1 [r bitconvert k string]
        assert_equal string [r type k]
        assert_equal "foobar" [r get k]
        assert_range [r ttl k] 900 1000
        assert_equal 0 [r bitconvert k string]
        assert_error {*syntax*} {r bitconvert k hash}
        # An empty string is an empty bitmap.
        r set k ""
        assert_equal 1 [r bitconvert k bitmap]
        assert_equal 0 [r bitcount k]
        assert_equal -1 [r bitpos k 1]
        assert_equal 1 [r bitconvert k string]
        assert_equal "" [r get k]
    }

    test {Bitmap: BITCONVERT creates an empty bitmap for a missing key} {
        r del k
        set dbsize [r dbsize]
        assert_equal 1 [r bitconvert k BITMAP]
        assert_equal {1 bitmap -1} [list [r exists k] [r type k] [r ttl k]]
        assert_equal [expr {$dbsize + 1}] [r dbsize]
        r set es ""
        foreach cmd {{bitcount KEY} {bitpos KEY 1} {bitpos KEY 0} {bitpos KEY 0 0 -1}} {
            assert_equal [r {*}[string map {KEY es} $cmd]] [r {*}[string map {KEY k} $cmd]]
        }
        assert_equal "" [bitmap_bytes k]
        assert_range [r memory usage k] 1 200
        assert_equal 0 [r bitconvert k bitmap]
        # SETBIT and BITFIELD grow it.
        r setbit k 9 1
        assert_equal "\x00\x40" [bitmap_bytes k]
        assert_equal 1 [r bitconvert k string]
        assert_equal "\x00\x40" [r get k]
    }

    test {Bitmap: BITCONVERT keyspace events} {
        r del k
        test_config_set r notify-keyspace-events En$
        set rd [valkey_deferring_client]
        psubscribe $rd {__keyevent@*}
        r bitconvert k bitmap
        r bitconvert k bitmap
        r bitconvert k string
        r bitconvert k string
        assert_match {pmessage * __keyevent@*__:new k} [$rd read]
        assert_match {pmessage * __keyevent@*__:bitconvert k} [$rd read]
        # No event when nothing changed, and no "new" when converting.
        assert_match {pmessage * __keyevent@*__:bitconvert k} [$rd read]
        r set marker 1
        assert_match {pmessage * __keyevent@*__:new marker} [$rd read]
        assert_match {pmessage * __keyevent@*__:set marker} [$rd read]
        $rd close
        bitmap_del k marker
    }

    test {Bitmap: BITCONVERT keeps the time to live in both directions} {
        r del k
        r bitconvert k bitmap
        r setbit k 3 1
        r pexpire k 100000
        r bitconvert k string
        assert_range [r pttl k] 90000 100000
        r bitconvert k bitmap
        assert_range [r pttl k] 90000 100000
    }

    test {Bitmap: BITCONVERT to string respects proto-max-bulk-len} {
        bitmap_new k
        r setbit k [expr {8 * 2000000}] 1
        test_config_set r proto-max-bulk-len 1048576
        assert_error {*proto-max-bulk-len*} {r bitconvert k string}
        assert_equal bitmap [r type k]
        r del k
    }

    test {Bitmap: BITCOUNT and BITPOS with ranges} {
        bitmap_twin_reset
        set rand [expr {srand(1234)}]
        for {set i 0} {$i < 2000} {incr i} {
            bitmap_twin setbit KEY [expr {int(rand() * 300000)}] 1
        }
        # A run of ones covering more than a whole chunk of 65536 bits.
        r setrange plain 50000 [string repeat "\xff" 8750]
        r del roar
        r restore roar 0 [r dump plain]
        r bitconvert roar bitmap
        bitmap_assert_same_value
        set len [r strlen plain]
        foreach {start end} [list 0 -1 0 0 -1 -1 5 3 100 [expr {$len - 1}] -9000 -100 \
                             49990 50010 40000 60000] {
            bitmap_twin bitcount KEY $start $end
            bitmap_twin bitcount KEY $start $end byte
            bitmap_twin bitcount KEY [expr {$start * 8 + 3}] [expr {$end * 8 + 5}] bit
            foreach bit {0 1} {
                bitmap_twin bitpos KEY $bit $start
                bitmap_twin bitpos KEY $bit $start $end
                bitmap_twin bitpos KEY $bit [expr {$start * 8 + 3}] [expr {$end * 8 + 5}] bit
            }
        }
        bitmap_twin bitcount KEY
        bitmap_twin bitpos KEY 0
        bitmap_twin bitpos KEY 1
        # Inside the run of ones.
        bitmap_twin bitpos KEY 0 [expr {400000 / 8 + 10}]
        bitmap_twin bitpos KEY 0 [expr {400000 / 8 + 10}] [expr {400000 / 8 + 20}]
    }

    test {Bitmap: BITPOS 0 on a bitmap of all ones} {
        bitmap_del plain roar
        r set plain [string repeat "\xff" 10000]
        r restore roar 0 [r dump plain]
        r bitconvert roar bitmap
        bitmap_twin bitpos KEY 0
        bitmap_twin bitpos KEY 0 0 -1
        bitmap_twin bitpos KEY 0 100
        bitmap_twin bitpos KEY 0 7 79999 bit
        bitmap_twin bitpos KEY 1 9999
    }

    test {Bitmap: BITOP} {
        foreach op {and or xor not} {
            bitmap_del pa{t} pb{t} pc{t}
            bitmap_new ra{t} rb{t} rc{t}
            for {set i 0} {$i < 300} {incr i} {
                foreach {p q} {pa{t} ra{t} pb{t} rb{t} pc{t} rc{t}} {
                    set bit [expr {int(rand() * 100000)}]
                    r setbit $p $bit 1
                    r setbit $q $bit 1
                }
            }
            if {$op eq {not}} {
                set plen [r bitop $op pdest{t} pa{t}]
                set rlen [r bitop $op rdest{t} ra{t}]
            } else {
                set plen [r bitop $op pdest{t} pa{t} pb{t} pc{t} nokey{t}]
                set rlen [r bitop $op rdest{t} ra{t} rb{t} rc{t} nokey{t}]
            }
            assert_equal $plen $rlen
            assert_equal string [r type pdest{t}]
            assert_equal bitmap [r type rdest{t}]
            assert_equal [r get pdest{t}] [bitmap_bytes rdest{t}]
            assert_equal [r bitcount pdest{t}] [r bitcount rdest{t}]
            # The destination as a source, for strings and bitmaps.
            if {$op eq {not}} {
                r bitop $op pa{t} pa{t}
                r bitop $op ra{t} ra{t}
            } else {
                r bitop $op pa{t} pa{t} pb{t}
                r bitop $op ra{t} ra{t} rb{t}
            }
            assert_equal [r get pa{t}] [bitmap_bytes ra{t}]
        }
        # A bitmap among string sources gives a bitmap, which GET rejects.
        r bitop or mixed{t} pa{t} rb{t} pc{t}
        assert_equal bitmap [r type mixed{t}]
        assert_error {*WRONGTYPE*} {r get mixed{t}}
        r bitop or pdest{t} pa{t} pb{t} pc{t}
        assert_equal [r get pdest{t}] [bitmap_bytes mixed{t}]
        # Only strings give a string.
        r bitop or sdest{t} pa{t} pb{t}
        assert_equal string [r type sdest{t}]
    }

    test {Bitmap: BITOP with only empty sources deletes the destination} {
        bitmap_new empty{t}
        r set dest{t} x
        assert_equal 0 [r bitop or dest{t} empty{t} nokey{t}]
        assert_equal 0 [r exists dest{t}]
        r set dest{t} x
        assert_equal 0 [r bitop not dest{t} empty{t}]
        assert_equal 0 [r exists dest{t}]
        r set dest{t} x
        assert_equal 0 [r bitop and dest{t} nokey1{t} nokey2{t}]
        assert_equal 0 [r exists dest{t}]
        # A string destination becomes a bitmap, and loses its TTL.
        r set dest{t} x ex 1000
        r setbit empty{t} 3 1
        assert_equal 1 [r bitop or dest{t} empty{t}]
        assert_equal {bitmap -1} [list [r type dest{t}] [r ttl dest{t}]]
    }

    test {Bitmap: BITFIELD} {
        bitmap_twin_reset
        foreach {type offset value} {u8 0 255 i16 5 -1000 u63 100 123456789 i64 65530 -1 u1 65535 1 i7 131071 -64} {
            bitmap_twin bitfield KEY set $type $offset $value
            bitmap_twin bitfield KEY get $type $offset
            bitmap_twin bitfield KEY incrby $type $offset 7
            bitmap_twin bitfield KEY overflow sat incrby $type $offset 1000000000
            bitmap_twin bitfield KEY overflow fail incrby $type $offset -1000000000
            bitmap_twin bitfield_ro KEY get $type [expr {$offset + 3}]
        }
        bitmap_assert_same_value
    }

    test {Bitmap: random commands match strings} {
        bitmap_twin_reset
        for {set i 0} {$i < 3000} {incr i} {
            set bit [expr {int(rand() * (rand() < 0.5 ? 1000 : 3000000))}]
            switch [expr {int(rand() * 5)}] {
                0 { bitmap_twin setbit KEY $bit [expr {int(rand() * 2)}] }
                1 { bitmap_twin getbit KEY $bit }
                2 { bitmap_twin bitcount KEY [expr {$bit / 64}] [expr {$bit / 8}] }
                3 { bitmap_twin bitpos KEY [expr {int(rand() * 2)}] [expr {$bit / 64}] }
                4 { bitmap_twin bitfield KEY incrby u13 $bit 1000 get i29 $bit }
            }
        }
        bitmap_assert_same_value
    }

    test {Bitmap: COPY makes an independent copy} {
        r del k2{t}
        bitmap_new k{t}
        r setbit k{t} 5 1
        r copy k{t} k2{t}
        assert_equal bitmap [r type k2{t}]
        r setbit k2{t} 6 1
        assert_equal 0 [r getbit k{t} 6]
        assert_equal 1 [r getbit k2{t} 6]
    }

    test {Bitmap: DUMP and RESTORE} {
        bitmap_new k
        r setbit k 70000 1
        r setbit k 3 1
        # The length extends past the last set bit.
        r setbit k 100000 0
        set bytes [bitmap_bytes k]
        set dump [r dump k]
        r del k
        r restore k 0 $dump
        assert_equal bitmap [r type k]
        assert_equal $bytes [bitmap_bytes k]
    }

    test {Bitmap: DEBUG RELOAD keeps the type and the value} {
        r del k
        bitmap_new empty
        r set k x
        r bitconvert k bitmap
        r setbit k 1234567 1
        r setbit k 12 1
        r setbit k 2000000 0
        set bytes [bitmap_bytes k]
        set digest [debug_digest_value k]
        r debug reload
        assert_equal bitmap [r type k]
        assert_equal $bytes [bitmap_bytes k]
        assert_equal $digest [debug_digest_value k]
        assert_equal bitmap [r type empty]
        assert_equal "" [bitmap_bytes empty]
    } {} {needs:debug}

    test {Bitmap: sparse bitmaps use little memory} {
        bitmap_new k
        r setbit k [expr {(1 << 32) - 1}] 1
        assert_equal bitmap [r type k]
        assert_range [r memory usage k] 1 1000
        assert_equal 1 [r bitcount k]
        assert_equal [expr {(1 << 32) - 1}] [r bitpos k 1]
        assert_equal [expr {(1 << 32) - 1}] [r bitpos k 1 -1 -1 bit]
        assert_equal [expr {(1 << 32) - 2}] [r bitpos k 0 -2 -1 bit]
        r del k
    }

    test {Bitmap: a bitmap cannot hold bits past 2^32 - 1} {
        test_config_set r proto-max-bulk-len 1073741824
        bitmap_new k
        r setbit k 1 1
        assert_error {*out of range for a bitmap*} {r setbit k [expr {1 << 32}] 1}
        assert_error {*out of range for a bitmap*} {r bitfield k set u8 [expr {(1 << 32) - 4}] 1}
        assert_equal bitmap [r type k]
        r del k
    }

    test {Bitmap: R.EXPORT writes the portable format, from strings and bitmaps} {
        foreach type {string bitmap} {
            r del k
            if {$type eq {bitmap}} { bitmap_new k }
            foreach bit {9 1 5} { r setbit k $bit 1 }
            assert_equal $type [r type k]
            assert_equal [roaring_array_payload 0 {1 5 9}] [r r.export k]
        }
        # A string encoded as an integer exports the bits of its digits.
        r set k 12345
        assert_encoding int k
        r set plain x
        r setrange plain 0 12345
        assert_encoding raw plain
        assert_equal [r r.export plain] [r r.export k]
        # Strings without set bits export the empty bitmap.
        r set k ""
        assert_equal [binary format ii 12346 0] [r r.export k]
        r set k [string repeat "\x00" 100]
        assert_equal [binary format ii 12346 0] [r r.export k]
        assert_equal {} [r r.export nosuchkey]
    }

    test {Bitmap: R.IMPORT reads the portable format} {
        r del k
        r rpush k a
        r expire k 1000
        # Like SET, it replaces any value and its time to live.
        assert_equal OK [r r.import k [roaring_array_payload 1 {3 7}]]
        assert_equal bitmap [r type k]
        assert_equal -1 [r ttl k]
        # Values 65539 and 65543: the bitmap ends with the byte of the last.
        assert_equal 2 [r bitcount k]
        assert_equal 65539 [r bitpos k 1]
        assert_equal 8193 [string length [bitmap_bytes k]]
        assert_equal {1 0 1} [list [r getbit k 65539] [r getbit k 65540] [r getbit k 65543]]
        # It exports the same bytes.
        assert_equal [roaring_array_payload 1 {3 7}] [r r.export k]
        # The empty bitmap gives a bitmap of zero bytes.
        assert_equal OK [r r.import k [binary format ii 12346 0]]
        assert_equal {1 bitmap 0} [list [r exists k] [r type k] [r bitcount k]]
    }

    test {Bitmap: R.EXPORT and R.IMPORT round trip} {
        foreach type {string bitmap} {
            bitmap_del k copy
            if {$type eq {bitmap}} { bitmap_new k }
            for {set i 0} {$i < 2000} {incr i} {
                r setbit k [expr {int(rand() * 3000000)}] 1
            }
            # A full chunk and a run, for run containers.
            for {set i 800000} {$i < 872000} {incr i 7} { r setbit k $i 1 }
            # The last byte holds a set bit, so the lengths are the same.
            r setbit k 3000000 1
            r r.import copy [r r.export k]
            assert_equal [r r.export k] [r r.export copy]
            assert_equal [r bitcount k] [r bitcount copy]
            if {$type eq {bitmap}} {
                assert_equal [debug_digest_value k] [debug_digest_value copy]
            }
        }
    }

    test {Bitmap: R.IMPORT rejects invalid payloads} {
        r del k
        set good [roaring_array_payload 0 {1 5 9}]
        foreach bad [list "" garbage [string range $good 0 end-1] "${good}x" [roaring_array_payload 0 {5 1 9}]] {
            assert_error {*invalid Roaring bitmap payload*} {r r.import k $bad}
        }
        assert_equal 0 [r exists k]
    }

    test {Bitmap: R.IMPORT respects proto-max-bulk-len} {
        set last [roaring_array_payload 65535 {65535}]
        set old [lindex [r config get proto-max-bulk-len] 1]
        test_config_set r proto-max-bulk-len 1048576
        assert_error {*proto-max-bulk-len*} {r r.import k $last}
        r config set proto-max-bulk-len $old
        if {$old >= (1 << 29)} {
            assert_equal OK [r r.import k $last]
            assert_equal 1 [r getbit k [expr {(1 << 32) - 1}]]
        }
        r del k
    }

    test {Bitmap: a script that disables replication propagates nothing} {
        r del sm{t} sf{t}
        bitmap_new sb{t}
        set repl [attach_to_replication_stream]
        r eval {
            server.set_repl(server.REPL_NONE)
            server.call('setbit', KEYS[1], 5, 1)
            server.call('setbit', KEYS[2], 5, 1)
            server.call('bitfield', KEYS[3], 'overflow', 'fail', 'incrby', 'u8', 800, 300)
        } 3 sb{t} sm{t} sf{t}
        r set marker 1
        assert_replication_stream $repl {
            {select *}
            {set marker 1}
        }
        close_replication_stream $repl
        assert_equal {bitmap string string} [list [r type sb{t}] [r type sm{t}] [r type sf{t}]]
    } {} {needs:repl}

    test {Bitmap: RESTORE rejects an invalid bitmap} {
        r del k2
        bitmap_new k
        r setbit k 100 1
        set dump [r dump k]
        # The payload is the RDB type, the length (13 bytes), then the
        # serialized bitmap as a string, then the RDB version and the CRC.
        # Claim a length of one byte, too short for bit 100.
        set bad [string replace $dump 1 1 "\x01"]
        r debug set-skip-checksum-validation 1
        test_defer {r debug set-skip-checksum-validation 0}
        assert_error {*Bad data format*} {r restore k2 0 $bad}
        # A corrupted serialization (its cookie).
        set bad [string replace $dump 5 5 "\x07"]
        assert_error {*Bad data format*} {r restore k2 0 $bad}
        assert_equal 0 [r exists k2]
    } {} {needs:debug}
}

start_server {tags {"bitmap large-memory"}} {
    test {Bitmap: results past 2^32 - 1 bits are strings} {
        test_config_set r proto-max-bulk-len 1073741824
        r del k{t} big{t}
        r setbit k{t} [expr {1 << 32}] 0
        assert_equal string [r type k{t}]
        # A BITOP with a bitmap source, but too long for a bitmap.
        r setbit big{t} [expr {1 << 32}] 1
        bitmap_new b{t}
        r setbit b{t} 3 1
        assert_equal [expr {(1 << 29) + 1}] [r bitop or k{t} big{t} b{t}]
        assert_equal string [r type k{t}]
        assert_equal 2 [r bitcount k{t}]
        assert_error {*too long for a bitmap*} {r bitconvert k{t} bitmap}
        r del k{t} big{t} b{t}
    }
}

start_server {tags {"bitmap aofrw external:skip"} overrides {appendonly yes aof-use-rdb-preamble no}} {
    test {Bitmap: the AOF rebuilds bitmaps, from commands and from a rewrite} {
        r flushall
        waitForBgrewriteaof r
        # A new bitmap, a converted string, a BITOP result, a bitmap whose
        # length extends past its last set bit, empty bitmaps, and a bitmap
        # only extended by a failed BITFIELD.
        r bitconvert a bitmap
        r setbit a 100 1
        r set b "foobar"
        r bitconvert b bitmap
        r setbit b 1000 1
        r bitconvert c bitmap
        r bitfield c set u8 0 7
        r set s1 "abc"
        r bitop or d s1 b
        r bitconvert e bitmap
        r setbit e 5 1
        r setbit e 4000 0
        r r.import f [binary format ii 12346 0]
        r bitconvert g bitmap
        r bitconvert h bitmap
        r bitfield h overflow fail incrby u8 800 300
        set digest [debug_digest]
        foreach k {a b c d e f g h} { assert_equal bitmap [r type $k] }
        assert_equal string [r type s1]

        # Replay the commands, then a rewrite.
        r debug loadaof
        assert_equal $digest [debug_digest]
        foreach k {a b c d e f g h} { assert_equal bitmap [r type $k] }
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
        assert_equal $digest [debug_digest]
        foreach k {a b c d e f g h} { assert_equal bitmap [r type $k] }
    } {} {needs:debug}
}

start_server {tags {"bitmap repl external:skip"}} {
    start_server {} {
        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        set replica [srv 0 client]

        test {Bitmap: a full sync carries bitmaps} {
            $primary bitconvert full bitmap
            $primary setbit full [expr {(1 << 31) - 1}] 1
            $primary bitconvert empty bitmap
            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }
            wait_for_ofs_sync $primary $replica
            assert_equal bitmap [$replica type full]
            assert_equal bitmap [$replica type empty]
            assert_range [$replica memory usage full] 1 1000
        }

        test {Bitmap: a replica holds the same types} {
            $primary bitconvert k bitmap
            for {set i 0} {$i < 1000} {incr i} {
                $primary setbit k [expr {int(rand() * 1000000)}] 1
            }
            $primary set conv "foobar"
            $primary bitconvert conv bitmap
            $primary setbit conv 1000 1
            $primary bitconvert bf bitmap
            $primary bitfield bf set u8 0 7
            $primary set s1 "abc"
            $primary bitop or dest s1 conv
            $primary bitop not k2 k
            $primary r.import k3 [$primary r.export k]
            $primary set back "xyz"
            $primary bitconvert back bitmap
            $primary bitconvert back string
            # Inside MULTI/EXEC.
            $primary multi
            $primary bitconvert m bitmap
            $primary setbit m 3 1
            $primary bitconvert m2 bitmap
            $primary exec
            # Bit writes on missing keys and BITOP of strings give strings.
            $primary setbit s2 10 1
            $primary bitop or s3 s1 s2
            # BITFIELD whose operations all fail, but that extends its key.
            $primary set bfs "ab"
            $primary bitconvert bfb bitmap
            $primary setbit bfb 1 1
            foreach k {bfm bfs bfb} {
                assert_equal {{}} [$primary bitfield $k overflow fail incrby u8 800 300]
            }
            wait_for_ofs_sync $primary $replica
            foreach k {k conv bf dest k2 k3 m m2 bfb} {
                assert_equal bitmap [$primary type $k]
                assert_equal bitmap [$replica type $k]
            }
            foreach k {back s2 s3 bfm bfs} {
                assert_equal string [$primary type $k]
                assert_equal string [$replica type $k]
            }
            assert_equal 101 [$replica strlen bfm]
            assert_equal [$primary debug digest] [$replica debug digest]
        } {} {needs:debug}
    }
}
