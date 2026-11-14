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

    test {DEL on a module type without free callback does not crash} {
        r flushall

        r typecb.setnofree key value
        assert_equal 1 [r del key]

        r typecb.setnofree key value
        assert_equal OK [r flushdb]
        r typecb.setnofree key value
        assert_equal OK [r flushall]

        r typecb.setnofree key value
        r pexpire key 1
        wait_for_condition 100 10 {
            [r exists key] == 0
        } else {
            fail "Key didn't expire."
        }

        assert_equal {PONG} [r ping]
    }

    test {DUMP of a module type without rdb_save callback does not crash} {
        r typecb.setnosave key value
        assert_error {ERR module type has no rdb_save callback} {r dump key}
        assert_equal {PONG} [r ping]
    }

    test {DEBUG OBJECT on a module type without rdb_save callback does not crash} {
        r typecb.setnosave key value
        assert_error "ERR module type has no rdb_save callback" {r debug object key}
        assert_match {Value at*} [r debug object key fast]
        assert_equal {PONG} [r ping]
    }

    test {BGSAVE with a module type without rdb_save callback does not crash the child} {
        r flushall

        set loglines [count_log_lines 0]
        r typecb.setnosave key value
        r bgsave
        waitForBgsave r
        assert_equal {err} [status r rdb_last_bgsave_status]
        verify_log_message 0 "*Module type 'no__save_' has no rdb_save callback, cannot serialize*" $loglines
        verify_log_message 0 "*Background saving error*" $loglines
        assert_equal {PONG} [r ping]
    }

    test {AOF rewrite with RDB preamble does not crash on a missing rdb_save callback} {
        r flushall
        r config set appendonly yes
        r config set aof-use-rdb-preamble yes
        waitForBgrewriteaof r

        set loglines [count_log_lines 0]
        r typecb.setnosave key value
        r bgrewriteaof
        waitForBgrewriteaof r
        assert_equal {err} [status r aof_last_bgrewrite_status]
        verify_log_message 0 "*Module type 'no__save_' has no rdb_save callback, cannot serialize*" $loglines
        verify_log_message 0 "*Background AOF rewrite terminated with error*" $loglines
        assert_equal {PONG} [r ping]
    }

    test {AOF rewrite does not crash on a missing aof_rewrite callback} {
        r flushall
        r config set appendonly yes
        r config set aof-use-rdb-preamble no
        waitForBgrewriteaof r

        set loglines [count_log_lines 0]
        r set plainkey plainvalue
        r typecb.setnorewrite modulekey modulevalue
        r bgrewriteaof
        waitForBgrewriteaof r
        assert_equal {err} [status r aof_last_bgrewrite_status]
        verify_log_message 0 "*Module type 'no__rwrt_' has no aof_rewrite callback, cannot rewrite*" $loglines
        verify_log_message 0 "*Background AOF rewrite terminated with error*" $loglines
        assert_equal {PONG} [r ping]
    }

    test {RESTORE of a module type without rdb_load callback does not crash} {
        r flushall
        r typecb.setnoload key value
        set payload [r dump key]
        r del key
        assert_error {ERR Bad data format*} {r restore key 0 $payload}
        assert_equal {PONG} [r ping]
    }

    test {ValkeyModule_SaveDataTypeToString does not crash on a missing rdb_save callback} {
        r flushall
        r typecb.setnosave key value
        assert_error {ERR save failed*} {r typecb.savetostring key}
        assert_equal {PONG} [r ping]
    }

    test {ValkeyModule_LoadDataTypeFromStringEncver does not crash on a missing rdb_load callback} {
        r flushall
        assert_error {ERR load failed*} {r typecb.loadfromstring payload}
        assert_equal {PONG} [r ping]
    }
}
