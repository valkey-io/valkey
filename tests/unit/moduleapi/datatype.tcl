set testmodule [file normalize tests/modules/datatype.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {DataType: Test module is sane, GET/SET work.} {
        r datatype.set dtkey 100 stringval
        assert {[r datatype.get dtkey] eq {100 stringval}}
    }

    test {test blocking of datatype creation outside of OnLoad} {
        assert_equal [r block.create.datatype.outside.onload] OK
    }

    test {DataType: RM_SaveDataTypeToString(), RM_LoadDataTypeFromStringEncver() work} {
        r datatype.set dtkey -1111 MyString
        set encoded [r datatype.dump dtkey]

        assert {[r datatype.restore dtkeycopy $encoded 4] eq {4}}
        assert {[r datatype.get dtkeycopy] eq {-1111 MyString}}
    }

    test {DataType: Handle truncated RM_LoadDataTypeFromStringEncver()} {
        r datatype.set dtkey -1111 MyString
        set encoded [r datatype.dump dtkey]
        set truncated [string range $encoded 0 end-1]

        catch {r datatype.restore dtkeycopy $truncated 4} e
        set e
    } {*Invalid*}

    test {DataType: ModuleTypeReplaceValue() happy path works} {
        r datatype.set key-a 1 AAA
        r datatype.set key-b 2 BBB

        assert {[r datatype.swap key-a key-b] eq {OK}}
        assert {[r datatype.get key-a] eq {2 BBB}}
        assert {[r datatype.get key-b] eq {1 AAA}}
    }

    test {DataType: ModuleTypeReplaceValue() fails on non-module keys} {
        r datatype.set key-a 1 AAA
        r set key-b RedisString

        catch {r datatype.swap key-a key-b} e
        set e
    } {*ERR*}

    test {DataType: Copy command works for modules} {
        # Test failed copies
        r datatype.set answer-to-universe 42 AAA
        catch {r copy answer-to-universe answer2} e
        assert_match {*module key failed to copy*} $e

        # Our module's data type copy function copies the int value as-is
        # but appends /<from-key>/<to-key> to the string value so we can
        # track passed arguments.
        r datatype.set sourcekey 1234 AAA
        r copy sourcekey targetkey
        r datatype.get targetkey
    } {1234 AAA/sourcekey/targetkey}

    test {DataType: Slow Loading} {
        r config set busy-reply-threshold 5000 ;# make sure we're using a high default
        # trigger slow loading
        r datatype.slow_loading 1
        set rd [valkey_deferring_client]
        set start [clock clicks -milliseconds]
        $rd debug reload

        # wait till we know we're blocked inside the module
        wait_for_condition 50 100 {
            [r datatype.is_in_slow_loading] eq 1
        } else {
            fail "Failed waiting for slow loading to start"
        }

        # make sure we get LOADING error, and that we didn't get here late (not waiting for busy-reply-threshold)
        assert_error {*LOADING*} {r ping}
        assert_lessthan [expr [clock clicks -milliseconds]-$start] 2000

        # abort the blocking operation
        r datatype.slow_loading 0
        wait_for_condition 50 100 {
            [s loading] eq {0}
        } else {
            fail "Failed waiting for loading to end"
        }
        $rd read
        $rd close
    }

    test {DataType: check the type name} {
        r flushdb
        r datatype.set foo 111 bar
        assert_type test___dt foo
    }

    test {SCAN module datatype} {
        r flushdb
        populate 1000
        r datatype.set foo 111 bar
        set type [r type foo]
        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur type $type]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        assert_equal 1 [llength $keys]    
    }

    test {SCAN module datatype with case-sensitive} {
        r flushdb
        populate 1000
        r datatype.set foo 111 bar
        set type "tEsT___dT"
        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur type $type]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        assert_equal 1 [llength $keys]
    }

    test {DataType: check memory usage} {
        r flushdb
        set large_key [string repeat A 100000]
        r datatype.set $large_key 111 bar
        assert_morethan [r memory usage $large_key] 100000
    }
}

start_server {tags {"modules external:skip"}} {
    r module load $testmodule

    test {valkey-check-rdb exits on an RDB truncated inside a module value} {
        # The marker is shorter than 20 bytes, so it is not LZF compressed
        # and we can find it in the RDB file.
        set marker MODULE_VALUE_MARKER
        r flushall
        r datatype.set dtkey 100 $marker
        r save

        set dir [lindex [r config get dir] 1]
        set rdb [file join $dir [lindex [r config get dbfilename] 1]]
        set fd [open $rdb r]
        fconfigure $fd -translation binary
        set data [read $fd]
        close $fd
        set idx [string first $marker $data]
        assert {$idx > 0}

        # Cut the file right after the string, before the module EOF opcode.
        set cut [expr {$idx + [string length $marker] - 1}]
        set truncated [file join $dir truncated-module.rdb]
        set out [file join $dir truncated-module.out]
        set fd [open $truncated w]
        fconfigure $fd -translation binary
        puts -nonewline $fd [string range $data 0 $cut]
        close $fd

        set pid [exec $::VALKEY_CHECK_RDB_BIN $truncated > $out 2>@1 &]
        wait_for_condition 50 100 {
            ![process_is_alive $pid]
        } else {
            catch {exec kill -9 $pid}
            fail "valkey-check-rdb hangs on a truncated module value"
        }
        set result [exec cat $out]
        assert_match {*RDB ERROR DETECTED*} $result
        assert_match {*Short read reading opcode from module*} $result
    }
}
