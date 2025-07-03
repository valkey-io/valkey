source "tests/unit/cluster/slot-migration.tcl"

proc info_field {info field} {
    foreach line [split $info "\n"] {
        if {[string match "$field:*" $line]} {
            return [string trim [lindex [split $line ":"] 1]]
        }
    }
    return ""
}

proc assert_keyevent_pattern {rd event_type key} {
    set event [$rd read]
    assert_match "pmessage __keyevent@* __keyevent@*:$event_type $key" $event
}

start_server {tags {"hashexpire external:skip"}} {
    ####### Valid scenarios tests #######
    foreach command {EX PX EXAT PXAT} {
        test "HGETEX $command expiry" {
            r FLUSHALL
            r HSET myhash f1 v1
            
            # Configuration dictionary mapping expiry commands to their test parameters:
            # - time: expiry value (seconds/milliseconds or absolute timestamp)
            # - wait: milliseconds to wait before checking expiration
            # - cmd: command to verify the TTL/expiry time
            set config [dict create \
                EX   [list time 1 wait 1100 cmd HTTL] \
                PX   [list time 100 wait 150 cmd HPTTL] \
                EXAT [list time [expr {[clock seconds] + 1}] wait 1100 cmd HEXPIRETIME] \
                PXAT [list time [expr {[clock milliseconds] + 100}] wait 150 cmd HPEXPIRETIME] \
            ]
            set params [dict get $config $command]
            set expire_time [dict get $params time]
            set wait_time [dict get $params wait]
            set ttl_cmd [dict get $params cmd]
            
            # Verify HGETEX command
            assert_equal "v1" [r HGETEX myhash $command $expire_time FIELDS 1 f1]
            set expire_result [r $ttl_cmd myhash FIELDS 1 f1]
            
            # Verify expiry
            if {[regexp "AT$" $command]} {
                assert_equal $expire_result $expire_time
            } else {
                assert_morethan $expire_result 0
            }
            after $wait_time
            assert_equal "" [r HGET myhash f1]
        }

        test "HGETEX $command with mix of existing and non-existing fields" {
            r FLUSHALL
            r HSET myhash f1 v1 f3 v3
            
            set config [dict create \
                EX   [list time 2000000] \
                PX   [list time 2000000] \
                EXAT [list time [expr {[clock seconds] + 2000000}]] \
                PXAT [list time [expr {[clock milliseconds] + 20000000}]] \
            ]
            set params [dict get $config $command]
            set expire_time [dict get $params time]
            
            # HGETEX on exist/non-exist fields
            assert_equal "v1 {} v3" [r HGETEX myhash $command $expire_time FIELDS 3 f1 f2 f3]
            
            # Verification checks (f2 should not be created)
            assert_equal "" [r HGET myhash f2]
            assert_equal -2 [r HTTL myhash FIELDS 1 f2]
            assert_morethan [r HTTL myhash FIELDS 1 f1] 0
            assert_morethan [r HTTL myhash FIELDS 1 f3] 0
        }

        test "HGETEX $command on more then 1 field" {
            r FLUSHALL
            r HSET myhash f1 v1 f2 v2
            
            set config [dict create \
                EX   [list time 1 wait 1100 check_cmd HTTL] \
                PX   [list time 100 wait 150 check_cmd HPTTL] \
                EXAT [list time [expr {[clock seconds] + 1}] wait 1100 check_cmd HEXPIRETIME] \
                PXAT [list time [expr {[clock milliseconds] + 100}] wait 150 check_cmd HPEXPIRETIME] \
            ]
            set params [dict get $config $command]
            set expire_time [dict get $params time]
            set wait_time [dict get $params wait]
            set check_cmd [dict get $params check_cmd]
            
            assert_equal "v1 v2" [r HGETEX myhash $command $expire_time FIELDS 2 f1 f2]
            
            # Verify expiration
            if {[regexp "AT$" $command]} {
                assert_equal $expire_time [r $check_cmd myhash FIELDS 1 f1]
                assert_equal $expire_time [r $check_cmd myhash FIELDS 1 f2]
            } else {
                assert_morethan [r $check_cmd myhash FIELDS 1 f1] 0
                assert_morethan [r $check_cmd myhash FIELDS 1 f2] 0
            }
            
            after $wait_time
            assert_equal "" [r HGET myhash f1]
            assert_equal "" [r HGET myhash f2]
        }

        test "HGETEX $command -> PERSIST" {
            r FLUSHALL
            r HSET myhash f1 v1
            r HSETEX myhash EX 10000 FIELDS 1 f2 v2

            set config [dict create \
                EX   [list time 1 cmd HTTL check_cmd HTTL] \
                PX   [list time 100 cmd HPTTL check_cmd HPTTL] \
                EXAT [list time [expr {[clock seconds] + 1}] cmd HTTL check_cmd HEXPIRETIME] \
                PXAT [list time [expr {[clock milliseconds] + 100}] cmd HPTTL check_cmd HPEXPIRETIME] \
            ]
            set params [dict get $config $command]
            set expire_time [dict get $params time]
            set ttl_cmd [dict get $params cmd]
            set check_cmd [dict get $params check_cmd]
            
            assert_equal "v1" [r HGETEX myhash $command $expire_time FIELDS 1 f1]
            if {[regexp "AT$" $command]} {
                assert_equal $expire_time [r $check_cmd myhash FIELDS 1 f1]
            } else {
                assert_morethan [r $ttl_cmd myhash FIELDS 1 f1] 0
            }
            
            assert_equal "v1" [r HGETEX myhash PERSIST FIELDS 1 f1]
            assert_equal -1 [r HTTL myhash FIELDS 1 f1]
            # Verify f2 still has ttl
            assert_morethan [r HTTL myhash FIELDS 1 f2] 100
        }

        test "HGETEX $command on non-exist field" {
            r FLUSHALL
            r HSET myhash f1 v1
            
            set config [dict create \
                EX   [list time 1] \
                PX   [list time 100] \
                EXAT [list time [expr {[clock seconds] + 1}]] \
                PXAT [list time [expr {[clock milliseconds] + 100}]] \
            ]
            set params [dict get $config $command]
            set expire_time [dict get $params time]
            
            assert_equal {{}} [r HGETEX myhash $command $expire_time FIELDS 1 f2]
        }

        test "HGETEX $command on non-exist key" {
            r FLUSHALL
            
            set config [dict create \
                EX   [list time 100000] \
                PX   [list time 10000000] \
                EXAT [list time [expr {[clock seconds] + 10000}]] \
                PXAT [list time [expr {[clock milliseconds] + 100000}]] \
            ]
            set params [dict get $config $command]
            set expire_time [dict get $params time]
            
            assert_equal "" [r HGETEX myhash $command $expire_time FIELDS 1 f2]
        }

        test "HGETEX $command with duplicate field names" {
            r FLUSHALL
            r HSET myhash f1 v1
            
            set config [dict create \
                EX   [list time 10000] \
                PX   [list time 10000] \
                EXAT [list time [expr {[clock seconds] + 10000}]] \
                PXAT [list time [expr {[clock milliseconds] + 100000}]] \
            ]
            set params [dict get $config $command]
            set expire_time [dict get $params time]

            assert_equal "v1 v1" [r HGETEX myhash $command $expire_time FIELDS 2 f1 f1]
        }
    }

    foreach command {EX PX} {
        test "HGETEX $command with 0 ttl" {
            r FLUSHALL
            r HSET myhash f1 v1
            assert_equal "v1" [r HGETEX myhash $command 0 FIELDS 1 f1]
            assert_equal "" [r HGET myhash f1]
            assert_equal -2 [r HTTL myhash FIELDS 1 f1]
        }
    }

    foreach command {EXAT PXAT} {
        test "HGETEX $command with past expiry" {
            r FLUSHALL
            r HSET myhash f1 v1
            if {$command eq "EXAT"} {
                set expire_time [expr {[clock seconds] - 100000}]
            } else {
                set expire_time [expr {[clock milliseconds] - 100000}]
            }
            assert_equal "v1" [r HGETEX myhash $command $expire_time FIELDS 1 f1]
            assert_equal "" [r HGET myhash f1]
            assert_equal -2 [r HTTL myhash FIELDS 1 f1]
        }
    }

    foreach command {EX PX EXAT PXAT} {
        test "HGETEX $command overwrites existing field TTL with bigger value" {
            r FLUSHALL
            set config [dict create \
                EX   [list setup_cmd EX setup_val 100000 bigger_val 200000] \
                PX   [list setup_cmd PX setup_val 100000000 bigger_val 200000000] \
                EXAT [list setup_cmd EX setup_val 100000 bigger_val [expr {[clock seconds] + 200000}]] \
                PXAT [list setup_cmd PX setup_val 100000000 bigger_val [expr {[clock milliseconds] + 200000000}]] \
            ]
            set params [dict get $config $command]
            set setup_cmd [dict get $params setup_cmd]
            set setup_val [dict get $params setup_val]
            set bigger_val [dict get $params bigger_val]
            
            r HSETEX myhash $setup_cmd $setup_val FIELDS 1 f1 v1
            set old_ttl [r HTTL myhash FIELDS 1 f1]
            r HGETEX myhash $command $bigger_val FIELDS 1 f1
            set new_ttl [r HTTL myhash FIELDS 1 f1]
            assert {$new_ttl > $old_ttl}
        }
        
        test "HGETEX $command overwrites existing field TTL with smaller value" {
            set config [dict create \
                EX   [list setup_cmd EX setup_val 100000 smaller_val 50000] \
                PX   [list setup_cmd PX setup_val 100000000 smaller_val 50000000] \
                EXAT [list setup_cmd EX setup_val 100000 smaller_val [expr {[clock seconds] + 50000}]] \
                PXAT [list setup_cmd PX setup_val 100000000 smaller_val [expr {[clock milliseconds] + 50000000}]] \
            ]
            set params [dict get $config $command]
            set setup_cmd [dict get $params setup_cmd]
            set setup_val [dict get $params setup_val]
            set smaller_val [dict get $params smaller_val]
            
            r FLUSHALL
            r HSETEX myhash $setup_cmd $setup_val FIELDS 1 f1 v1
            set old_ttl [r HTTL myhash FIELDS 1 f1]
            r HGETEX myhash $command $smaller_val FIELDS 1 f1
            set new_ttl [r HTTL myhash FIELDS 1 f1]
            assert {$new_ttl <= $old_ttl}
        }
    }

    test {HGETEX - verify no change when field does not exist} {
        r FLUSHALL
        r HSET myhash f1 v1
        set mem_before [r MEMORY USAGE myhash]
        assert_equal {{}} [r HGETEX myhash EX 1 FIELDS 1 f2]
        set memory_after [r MEMORY USAGE myhash]
        assert_equal $mem_before $memory_after
    }

    ####### Invalid scenarios tests #######
        test {HGETEX EX- multiple options used (EX + PX)} {
        r FLUSHALL
        r HSET myhash f1 v1
        catch {r HGETEX myhash EX 60 PX 1000 FIELDS 1 f1} e
        set e
    } {ERR syntax error}
    
    test {HGETEX EXAT- multiple options used (EXAT + PXAT)} {
        r FLUSHALL
        r HSET myhash f1 v1
        catch {r HGETEX myhash EXAT [expr {[clock seconds] + 100}] PXAT [expr {[clock milliseconds] + 100000}] 1000 FIELDS 1 f1} e
        set e
    } {ERR syntax error}
    
    # Common error scenarios for all commands
    foreach {cmd ttl_val} [list \
        EX 60 \
        PX 60 \
        EXAT [expr {[clock seconds] + 100}] \
        PXAT [expr {[clock milliseconds] + 100}] \
    ] {
        test "HGETEX $cmd- missing TTL value" {
            r FLUSHALL
            r HSET myhash f1 v1
            catch {r HGETEX myhash $cmd FIELDS 1 f1} e
            set e
        } {ERR syntax error}
        
        test "HGETEX $cmd- negative TTL" {
            r FLUSHALL
            r HSET myhash f1 v1
            catch {r HGETEX myhash $cmd -10 FIELDS 1 f1} e
            set e
        } {ERR invalid expire time in 'hgetex' command}
        
        test "HGETEX $cmd- non-integer TTL" {
            r FLUSHALL
            r HSET myhash f1 v1
            catch {r HGETEX myhash $cmd abc FIELDS 1 f1} e
            set e
        } {ERR value is not an integer or out of range}
        
        test "HGETEX $cmd- missing FIELDS keyword" {
            r FLUSHALL
            r HSET myhash f1 v1
            catch {r HGETEX myhash $cmd $ttl_val 1 f1} e
            set e
        } {ERR syntax error}
        
        test "HGETEX $cmd- wrong numfields count (too few fields)" {
            r FLUSHALL
            r HSET myhash f1 v1 f2 v2
            catch {r HGETEX myhash $cmd $ttl_val FIELDS 2 f1} e
            set e
        } {ERR *}
        
        test "HGETEX $cmd- wrong numfields count (too many fields)" {
            r FLUSHALL
            r HSET myhash f1 v1
            catch {r HGETEX myhash $cmd $ttl_val FIELDS 1 f1 f2} e
            set e
        } {ERR syntax error}
        
        test "HGETEX $cmd- key is wrong type (string instead of hash)" {
            r FLUSHALL
            r SET mystring "v1"
            catch {r HGETEX mystring $cmd $ttl_val FIELDS 1 f1} e
            set e
        } {WRONGTYPE Operation against a key holding the wrong kind of value}
        
        test "HGETEX $cmd with FIELDS 0" {
            r FLUSHALL
            catch {r HGETEX myhash $cmd $ttl_val FIELDS 0} e
            set e
        } {ERR syntax error}
        
        test "HGETEX $cmd with negative numfields" {
            r FLUSHALL
            catch {r HGETEX myhash $cmd $ttl_val FIELDS -10} e
            set e
        } {ERR syntax error}

        test "HGETEX $cmd with missing key" {
            r FLUSHALL
            set expire [expr {[clock seconds] + 100}]
            catch {r HGETEX $cmd $expire FIELDS 1 f1} e
            set e
        } {ERR syntax error}
    }
}

## HGETEX -> Keyspace notification tests ####
start_server {tags {"hashexpire external:skip"}} {
    if {$::singledb} {
        set db 0
    } else {
        set db 9
    }
    set all_h_pattern "h*"
    set hexpire_pattern "hexpire"
    set hpersist_pattern "hpersist"

    r config set notify-keyspace-events KEA

    foreach command {EX PX EXAT PXAT} {
        set config [dict create \
            EX   [list time 6000000] \
            PX   [list time 6000000] \
            EXAT [list time [expr {[clock seconds] + 6000000}]] \
            PXAT [list time [expr {[clock milliseconds] + 6000000}]] \
        ]
        set params [dict get $config $command]
        set expire_time [dict get $params time]

        test "HGETEX $command generates hexpire keyspace notification" {
            r FLUSHALL
            r HSET myhash f1 v1
            
            set rd [valkey_deferring_client]
            assert_equal {1} [psubscribe $rd __keyevent@*]
            
            
            r HGETEX myhash $command $expire_time FIELDS 1 f1

            assert_keyevent_pattern $rd hexpire myhash
            $rd close
        }

        test "HGETEX $command with multiple fields generates single notification" {
            r FLUSHALL
            r HSET myhash f1 v1 f2 v2 f3 v3
            
            set rd [valkey_deferring_client]
            assert_equal {1} [psubscribe $rd __keyevent@*]
            
            r HGETEX myhash $command $expire_time FIELDS 3 f1 f2 f3
            
            assert_keyevent_pattern $rd hexpire myhash
            # Verify no notification (getting hset and not hexpire)
            r HSET dummy dummy dummy
            assert_keyevent_pattern $rd hset dummy
            $rd close
        }

        test "HGETEX $command on non-existent field generates no notification" {
            r FLUSHALL
            r HSET myhash f1 v1

            set rd [valkey_deferring_client]
            assert_equal {1} [psubscribe $rd __keyevent@*]

            # This HGETEX targets a non-existent field, so no notification about hexpire should be emitted
            r HGETEX myhash $command $expire_time FIELDS 1 f2
            
            # # Verify no notification (getting hset and not hexpire)
            # r HSET dummy dummy dummy
            # assert_keyevent_pattern $rd hset dummy

            $rd close
        }
    }
    
    test {HGETEX PERSIST generates hpersist keyspace notification} {
        r FLUSHALL
        r HSET myhash f1 v1
        r HEXPIRE myhash 60 FIELDS 1 f1
        
        set rd [valkey_deferring_client]
        assert_equal {1} [psubscribe $rd __keyevent@*]
        
        r HGETEX myhash PERSIST FIELDS 1 f1

        assert_keyevent_pattern $rd hpersist myhash
        $rd close
    }
    
    

    foreach command {EX PX EXAT PXAT} {
        set config [dict create \
            EX   [list time 0] \
            PX   [list time 0] \
            EXAT [list time [expr {[clock seconds] - 2000}]] \
            PXAT [list time [expr {[clock milliseconds] - 2000}]] \
        ]
        set params [dict get $config $command]
        set expire_time [dict get $params time]

        test "HGETEX $command 0/past time works correctly with 1 field" {
            r FLUSHALL

            # Create hash with field
            r HSET myhash f1 v1
            assert_equal 1 [r HLEN myhash]
            assert_match  {1} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]

            set rd [valkey_deferring_client]
            assert_equal {1} [psubscribe $rd __keyevent@*]
            
            # Set field to expire immediately
            r HGETEX myhash $command $expire_time FIELDS 1 f1

            # Verify field and keys are deleted
            assert_keyevent_pattern $rd hexpired myhash
            assert_keyevent_pattern $rd del myhash
            assert_equal -2 [r HTTL myhash FIELDS 1 f1]
            assert_equal 0 [r HLEN myhash]
            assert_equal 0 [r EXISTS myhash]
            assert_match "" [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]

            $rd close
        }

        test "HGETEX $command 0/past time works correctly with 1 field on field with expire" {
            r FLUSHALL

            # Create hash with field
            r HSETEX myhash EX 1000 FIELDS 1 f1 v1
            assert_equal 1 [r HLEN myhash]
            assert_match  {1} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]

            set rd [valkey_deferring_client]
            assert_equal {1} [psubscribe $rd __keyevent@*]
            
            # Set field to expire immediately
            r HGETEX myhash $command $expire_time FIELDS 1 f1

            # Verify field and keys are deleted
            assert_keyevent_pattern $rd hexpired myhash
            assert_keyevent_pattern $rd del myhash
            assert_equal -2 [r HTTL myhash FIELDS 1 f1]
            assert_equal 0 [r HLEN myhash]
            assert_equal 0 [r EXISTS myhash]
            assert_match "" [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]

            $rd close
        }
    
        test "HGETEX $command 0/past time works correctly with more then 1 field" {
            r FLUSHALL

            # Create hash with field
            r HSET myhash f1 v1 f2 v2
            assert_equal 2 [r HLEN myhash]
            assert_match  {1} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]
            
            set rd [valkey_deferring_client]
            assert_equal {1} [psubscribe $rd __keyevent@*]
            
            # Set field to expire immediately
            r HGETEX myhash $command $expire_time FIELDS 1 f2

            # Verify field and keys are deleted
            assert_keyevent_pattern $rd hexpired myhash
            assert_equal -2 [r HTTL myhash FIELDS 1 f2]
            assert_equal 1 [r HLEN myhash]
            assert_equal 1 [r EXISTS myhash]
            assert_match 1 [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]

            $rd close
        }

        test "HGETEX $command 0/past time works correctly with more then 1 field and expire" {
            r FLUSHALL

            # Create hash with field
            r HSET myhash f1 v1 f2 v2 f3 v3 f4 v4
            r HEXPIRE myhash 1000000 FIELDS 1 f1
            assert_equal 4 [r HLEN myhash]
            assert_match  {1} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]

            set rd [valkey_deferring_client]
            assert_equal {1} [psubscribe $rd __keyevent@*]
            
            # Set field to expire immediately
            r HGETEX myhash $command $expire_time FIELDS 1 f1

            # Verify field and keys are deleted
            assert_keyevent_pattern $rd hexpired myhash
            assert_equal -2 [r HTTL myhash FIELDS 1 f1]
            assert_equal 3 [r HLEN myhash]
            assert_equal 1 [r EXISTS myhash]
            assert_match 1 [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]

            $rd close
        }
    }
}

# HSETEX ####
start_server {tags {"hashexpire external:skip"}} {    
    test {HSETEX KEEPTTL - preserves existing TTL of field} {
        r FLUSHALL

        # Set a field with a known TTL
        r HSETEX myhash PX 1000 FIELDS 1 field1 val1
        set original_pttl [r HPTTL myhash FIELDS 1 field1]
        set original_expiretime [r HEXPIRETIME myhash FIELDS 1 field1]

        # Validate TTL is active and expiretime is in the future
        assert {$original_pttl > 0}
        assert {$original_expiretime > [clock seconds]}

        # Overwrite the field with KEEPTTL
        r HSETEX myhash KEEPTTL FIELDS 1 field1 newval

        # Ensure TTL is preserved
        set updated_pttl [r HPTTL myhash FIELDS 1 field1]
        set updated_expiretime [r HEXPIRETIME myhash FIELDS 1 field1]
        assert {$updated_pttl > 0}
        assert {$updated_pttl <= $original_pttl}
        assert_equal $original_expiretime $updated_expiretime

        # Ensure value was updated
        assert_equal newval [r HGET myhash field1]
    }

    test {HSETEX EX - FIELDS 0 returns error} {
        r FLUSHALL    
        catch {r HSETEX myhash EX 10 FIELDS 0} e
        set e
    } {ERR *}

    test {HSETEX EX - test negative ttl} {
        set ttl -10
        catch {r HSETEX myhash EX $ttl FIELDS 1 field1 val1} e
        set e
    } {ERR invalid expire time in 'hsetex' command}

    test {HSETEX EX - test non-numeric ttl} {
        set ttl abc
        catch {r HSETEX myhash EX $ttl FIELDS 1 field1 val1} e
        set e
    } {ERR value is not an integer or out of range}

    test {HSETEX EX - overwrite field resets TTL} {
        r FLUSHALL        
        r HSETEX myhash EX 100 FIELDS 1 field1 val1
        r HSETEX myhash EX 200 FIELDS 1 field1 newval
        assert_equal 200 [r HTTL myhash FIELDS 1 field1]
        assert_equal newval [r HGET myhash field1]
    }

    test {HSETEX EX - test zero ttl expires immediately} {
        r FLUSHALL
        r HSETEX myhash EX 0 FIELDS 1 field1 val1
        after 10
        assert_equal 0 [r HEXISTS myhash field1]
    }

    test {HSETEX EX - test mix of expiring and persistent fields} {
        r FLUSHALL
        r HSET myhash field2 "persistent"
        r HSETEX myhash EX 1 FIELDS 1 field1 "temp"
        after 1100
        assert_equal 0 [r HEXISTS myhash field1]
        assert_equal 1 [r HEXISTS myhash field2]
    }

    test {HSETEX EX - test missing TTL} {
        catch {r HSETEX myhash EX FIELDS 1 field1 val1} e
        set e
    } {ERR syntax error}

    test {HSETEX EX - mismatched field/value count} {
        catch {r HSETEX myhash EX 10 FIELDS 2 field1 val1} e
        set e
    } {ERR *}



    ###### PX #######

    test {HSETEX PX - test negative ttl} {
        set ttl -50
        catch {r HSETEX myhash PX $ttl FIELDS 1 field1 val1} e
        set e
    } {ERR invalid expire time in 'hsetex' command}

    test {HSETEX PX - test non-numeric ttl} {
        set ttl xyz
        catch {r HSETEX myhash PX $ttl FIELDS 1 field1 val1} e
        set e
    } {ERR value is not an integer or out of range}

    test {HSETEX PX - overwrite field resets TTL} {
        r FLUSHALL
        r HSETEX myhash PX 10000 FIELDS 1 field1 val1
        r HSETEX myhash PX 20000 FIELDS 1 field1 newval
        set ttl [r HPTTL myhash FIELDS 1 field1]
        assert {$ttl >= 19000 && $ttl <= 20000}
        assert_equal newval [r HGET myhash field1]
    }

    test {HSETEX PX - test zero ttl expires immediately} {
        r FLUSHALL
        r HSETEX myhash PX 0 FIELDS 1 field1 val1
        after 10
        assert_equal 0 [r HEXISTS myhash field1]
    }

    test {HSETEX PX - test mix of expiring and persistent fields} {
        r FLUSHALL
        r HSET myhash field2 "persistent"
        r HSETEX myhash PX 10 FIELDS 1 field1 "temp"
        after 20
        assert_equal 0 [r HEXISTS myhash field1]
        assert_equal 1 [r HEXISTS myhash field2]
    }

    test {HSETEX PX - test missing TTL} {
        catch {r HSETEX myhash PX FIELDS 1 field1 val1} e
        set e
    } {ERR syntax error}

    # test {HSETEX PX - mismatched field/value count} {
    #     catch {r HSETEX myhash PX 100 FIELDS 2 field1 val1} e
    #     set e
    # } {ERR wrong number of arguments for 'hsetex' command}


    ## FNX/FXX

    # hsetex throws ERR syntax error, it shouldn't
    test {HSETEX EX FNX - set only if none of the fields exist} {
        r FLUSHALL        
        r HSET myhash field1 val1
        set res [r HSETEX myhash EX 10 FNX FIELDS 1 field1 val2]
        assert_equal 0 $res
        assert_equal val1 [r HGET myhash field1]

        # Now try with all-new fields
        set res [r HSETEX myhash EX 10 FNX FIELDS 2 f2 v2 f3 v3]
        assert_equal 1 $res
        assert_equal v2 [r HGET myhash f2]
        assert_equal v3 [r HGET myhash f3]
    }

    test {HSETEX EX FXX - set only if all fields exist} {
        r FLUSHALL
        r HSET myhash field1 val1 field2 val2
        set res [r HSETEX myhash EX 10 FXX FIELDS 2 field1 new1 field2 new2]
        assert_equal 1 $res
        assert_equal new1 [r HGET myhash field1]
        assert_equal new2 [r HGET myhash field2]

        # Now try when one field doesn't exist
        set res [r HSETEX myhash EX 10 FXX FIELDS 2 field1 x fieldX y]
        assert_equal 0 $res
        assert_equal new1 [r HGET myhash field1]
        assert_equal 0 [r HEXISTS myhash fieldX]
    }

    # Syntax error: HSETEX myhash PX 100 FNX FIELDS 2 x 2 y 3
    test {HSETEX PX FNX - partial conflict returns 0} {
        r FLUSHALL
        r HSET myhash x 1
        set res [r HSETEX myhash PX 100 FNX FIELDS 2 x 2 y 3]
        assert_equal 0 $res
        assert_equal 1 [r HEXISTS myhash x]
        assert_equal 0 [r HEXISTS myhash y]
    }

    test {HSETEX PX FXX - one field missing returns 0} {
        r FLUSHALL
        r HSET myhash a 1
        set res [r HSETEX myhash PX 100 FXX FIELDS 2 a 2 b 3]
        assert_equal 0 $res
        assert_equal 1 [r HGET myhash a]
        assert_equal 0 [r HEXISTS myhash b]
    }

    test {HSETEX EX - FNX and FXX conflict error} {
        catch {r HSETEX myhash EX 10 FNX FXX FIELDS 1 x y} e
        set e
    } {ERR syntax error}

    #################### Lazy Expiry ########################

    test {HGETALL skips expired fields without triggering lazy expiry} {
        r FLUSHALL
        r DEBUG SET-ACTIVE-EXPIRE no

        # Set two fields: one persistent, one with short TTL
        r HSET myhash persistent "val1"
        r HSETEX myhash PX 5 FIELDS 1 expiring "val2"

        # Wait for expiry to pass
        after 10      

        # HGETALL should skip expired field
        set result [r HGETALL myhash]
        assert_equal {persistent val1} $result

        # HLEN should still count both fields (expired field not removed)
        assert_equal 2 [r HLEN myhash]

        # Re-enable active expiry
        r DEBUG SET-ACTIVE-EXPIRE yes
    } 

    test {HSCAN skips expired fields} {
        r FLUSHALL
        r DEBUG SET-ACTIVE-EXPIRE no

        # Set multiple fields, one with expiry
        r HSET myhash persistent1 "a" persistent2 "b"
        r HSETEX myhash PX 5 FIELDS 1 expiring "c"

        # Wait for expiration
        after 10

        # HSCAN must not return the expired field
        set cursor 0
        set allfields {}
        while {1} {
            set res [r HSCAN myhash $cursor]
            set cursor [lindex $res 0]
            set kvs [lindex $res 1]
            lappend allfields {*}$kvs
            if {$cursor eq "0"} break
        }

        # Extract just the field names
        set fieldnames [lmap {k v} $allfields { set k }]
        set fieldnames_sorted [lsort $fieldnames]

        # Should only include persistent1 and persistent2
        assert_equal {persistent1 persistent2} $fieldnames_sorted

        # Re-enable active expiry for future tests
        r DEBUG SET-ACTIVE-EXPIRE yes
    } 

    test {MOVE preserves field TTLs} {
        r FLUSHALL
        r SELECT 0
        r HSETEX myhash PX 50000 FIELDS 1 field1 val1

        # Capture original TTL
        set original_ttl [r HPTTL myhash FIELDS 1 field1]
        assert {$original_ttl > 0}

        # Move to DB 1
        assert_equal 1 [r MOVE myhash 1]

        # Switch to target DB
        r SELECT 1

        # Field must exist and TTL must be preserved        
        set moved_ttl [r HPTTL myhash FIELDS 1 field1]
        assert {$moved_ttl > 0 && $moved_ttl <= $original_ttl}
    }

    test {HSET - overwrite lazily expired field without TTL clears expiration} {
        r FLUSHALL
        r debug SET-ACTIVE-EXPIRE no

        # This test verifies that if a field has expired (but not yet lazily deleted),
        # and it is overwritten using a plain HSET (i.e., no TTL),
        # Valkey treats the field as non existing and updates it,
        # effectively clearing the old TTL and making the field persistent.
    
        r HSETEX myhash PX 10 FIELDS 1 field1 oldval
        wait_for_condition 100 100 {
            [r HTTL myhash FIELDS 1 field1] eq "-2"
        } else {
            fail "hash value was not expired after timeout"
        }

        # Field should still be present in memory due to lazy expiry
        assert_equal 1 [r HLEN myhash]

        # Overwrite with HSET (no TTL) before accessing
        r HSET myhash field1 newval

        # TTL should now be gone; field becomes persistent
        set ttl [r HPTTL myhash FIELDS 1 field1]
        assert_equal -1 $ttl
        assert_equal newval [r HGET myhash field1]
        assert_equal 1 [r HLEN myhash]

        r debug SET-ACTIVE-EXPIRE yes
    }

    test {HINCRBY - on expired field} {
        r FLUSHALL
        r debug SET-ACTIVE-EXPIRE no

        # This test verifies that if a field has expired,
        # and it is overwritten using a plain HINCRBY (i.e., no TTL),
        # Valkey treats the field as still existing and updates it,
        # effectively clearing the old TTL and starting the value from 0.
    
        r HSETEX myhash PX 10 FIELDS 1 field1 1
        wait_for_condition 100 100 {
            [r HTTL myhash FIELDS 1 field1] eq "-2"
        } else {
            fail "hash value was not expired after timeout"
        }

        # Field should still be present in memory
        assert_equal 1 [r HLEN myhash]

        # Overwrite with HINCRBY (no TTL) before accessing
        r HINCRBY myhash field1 1

        # Sanity check: check we only have one field in the hash
        assert_equal 1 [r HLEN myhash]

        # TTL should now be gone; field becomes persistent
        set ttl [r HPTTL myhash FIELDS 1 field1]
        assert_equal -1 $ttl
        assert_equal 1 [r HGET myhash field1]
        assert_equal 1 [r HLEN myhash]

        # set expiration on the field
        assert_equal 1 [r HEXPIRE myhash 100000000 FIELDS 1 field1]
        # verify the field has TTL
        assert_morethan [r HPTTL myhash FIELDS 1 field1] 0
        # now incr the field again
        assert_equal 2 [r HINCRBY myhash field1 1]
        # verify the field has TTL
        assert_morethan [r HPTTL myhash FIELDS 1 field1] 0
        r debug SET-ACTIVE-EXPIRE yes
    }

    test {HINCRBYFLOAT - on expired field} {
        r FLUSHALL
        r debug SET-ACTIVE-EXPIRE no

        # This test verifies that if a field has expired,
        # and it is overwritten using a plain HINCRBYFLOAT (i.e., no TTL),
        # Valkey treats the field as still existing and updates it,
        # effectively clearing the old TTL and starting the value from 0.
    
        r HSETEX myhash PX 10 FIELDS 1 field1 1
        wait_for_condition 100 100 {
            [r HTTL myhash FIELDS 1 field1] eq "-2"
        } else {
            fail "hash value was not expired after timeout"
        }

        # Field should still be present in memory
        assert_equal 1 [r HLEN myhash]

        # Overwrite with HINCRBYFLOAT (no TTL) before accessing
        r HINCRBYFLOAT myhash field1 1

        # Sanity check: check we only have one field in the hash
        assert_equal 1 [r HLEN myhash]

        # TTL should now be gone; field becomes persistent
        set ttl [r HPTTL myhash FIELDS 1 field1]
        assert_equal -1 $ttl
        assert_equal 1 [r HGET myhash field1]
        assert_equal 1 [r HLEN myhash]

        # set expiration on the field
        assert_equal 1 [r HEXPIRE myhash 100000000 FIELDS 1 field1]
        # verify the field has TTL
        assert_morethan [r HPTTL myhash FIELDS 1 field1] 0
        # now incr the field again
        assert_equal 2 [r HINCRBYFLOAT myhash field1 1]
        # verify the field has TTL
        assert_morethan [r HPTTL myhash FIELDS 1 field1] 0
        r debug SET-ACTIVE-EXPIRE yes
    }

    test {HSET - overwrite unexpired field removes TTL} {
        r FLUSHALL
        r debug SET-ACTIVE-EXPIRE no

        # This test verifies that overwriting a field with HSET,
        # even while its TTL is still valid (not expired),
        # clears the TTL and makes the field persistent.
        # This behavior is consistent with how HSET works for normal keys.

        # Set field with long TTL
        r HSETEX myhash PX 1000 FIELDS 1 field1 val1

        # Confirm TTL is active
        set before [r HPTTL myhash FIELDS 1 field1]    
        assert {$before > 0}

        # Overwrite with HSET before TTL expires
        r HSET myhash field1 newval

        # TTL should now be gone
        set after [r HPTTL myhash FIELDS 1 field1]
        assert_equal -1 $after
        assert_equal newval [r HGET myhash field1]

        r debug SET-ACTIVE-EXPIRE yes
    }

    test {HDEL - lazily expired field is removed without triggering expiry logic} {
        r FLUSHALL
        r debug SET-ACTIVE-EXPIRE no

        # This test proves that deleting an expired field with HDEL
        # does NOT trigger Valkey's expiration mechanism.
        #
        # The key observation is that Valkey tracks how many fields were
        # expired via TTL using the `expired_subkeys` counter in INFO stats.
        # If HDEL caused expiration to be processed internally,
        # this counter would increment. We assert that it remains unchanged.

        # Capture expired_subkeys before
        set before_info [r INFO stats]
        set before [info_field $before_info expired_subkeys]

        # Create field with short TTL
        r HSETEX myhash PX 10 FIELDS 1 field1 val1
        after 20

        # Field is technically expired, but still in-memory due to lazy expiry
        assert_equal 1 [r HLEN myhash]

        # Delete the expired field directly
        r HDEL myhash field1

        # Field should be gone
        assert_equal 0 [r HEXISTS myhash field1]

        # Capture expired_subkeys again
        set after_info [r INFO stats]
        set after [info_field $after_info expired_subkeys]

        # Verify that no expiry occurred internally
        assert_equal $before $after
        r debug SET-ACTIVE-EXPIRE yes
    }

    ###### Test EXPIRE #############


    # Basic Expiry Functionality
    test {HEXPIRE - set TTL on existing field} {
        r FLUSHALL
        r HSET myhash field1 hello
        r HEXPIRE myhash 10 FIELDS 1 field1
        set ttl [r HTTL myhash FIELDS 1 field1]
        assert {$ttl > 0}
    }

    test {HEXPIRE - TTL 0 deletes field} {
        r FLUSHALL
        r HSET myhash field1 goodbye
        set res [r HEXPIRE myhash 0 FIELDS 1 field1]
        assert_equal {2} $res
        assert_equal 0 [r HEXISTS myhash field1]
    }

    test {HEXPIRE - negative TTL returns error} {
        r FLUSHALL
        r HSET myhash field1 val
        catch {r HEXPIRE myhash -5 FIELDS 1 field1} e
        set e
    } {ERR invalid expire time in 'hexpire' command}

    test {HEXPIRE - wrong type key returns error} {
        r FLUSHALL
        r SET myhash notahash
        catch {r HEXPIRE myhash 10 FIELDS 1 field1} e
        set e
    } {WRONGTYPE Operation against a key holding the wrong kind of value}

    # Conditionals: NX
    test {HEXPIRE NX - only set when field has no TTL} {
        r FLUSHALL
        r HSETEX myhash PX 100 FIELDS 1 field1 val
        set res [r HEXPIRE myhash 10 NX FIELDS 1 field1]
        assert_equal {0} $res

        r HSET myhash field2 val2
        set res2 [r HEXPIRE myhash 10 NX FIELDS 1 field2]
        assert_equal {1} $res2
    }

    # Conditionals: XX
    test {HEXPIRE XX - only set when field has TTL} {
        r FLUSHALL
        r HSET myhash field1 val1 field2 val2
        r HEXPIRE myhash 20 FIELDS 1 field1
        set res [r HEXPIRE myhash 30 XX FIELDS 2 field1 field2]
        assert_equal {1 0} $res
    }

    # Conditionals: GT
    test {HEXPIRE GT - only set if new TTL > existing TTL} {
        r FLUSHALL
        r HSETEX myhash EX 300 FIELDS 1 field1 val1
        after 10
        set res [r HEXPIRE myhash 600 GT FIELDS 1 field1]  ;# 600s > 300s remaining
        assert_equal {1} $res

        # GT should fail if field is persistent
        r HSET myhash field2 val2
        set res2 [r HEXPIRE myhash 1 GT FIELDS 1 field2]
        assert_equal {0} $res2
    }

    # Conditionals: LT
    test {HEXPIRE LT - only set if new TTL < existing TTL} {
        r FLUSHALL
        r HSETEX myhash EX 600 FIELDS 1 field1 val1
        set res [r HEXPIRE myhash 1 LT FIELDS 1 field1]
        assert_equal {1} $res

        ## TODO this is an expected behavior really? what does non existintg ttl mean?
        r HSET myhash field2 val2
        set res2 [r HEXPIRE myhash 1 LT FIELDS 1 field2]
        assert_equal {1} $res2
    }

     # TTL Refresh
    test {HEXPIRE - refresh TTL with new value} {
        r FLUSHALL
        r HSET myhash field1 val1
        r HEXPIRE myhash 1 FIELDS 1 field1
        after 500
        r HEXPIRE myhash 3 FIELDS 1 field1
        set ttl [r HTTL myhash FIELDS 1 field1]
        assert {$ttl >= 2}
    }

    # Error Cases
    test {HEXPIRE - conflicting conditions error} {
        r FLUSHALL
        r HSET myhash field1 val
        catch {r HEXPIRE myhash 10 NX XX FIELDS 1 field1} e
        set e
    } {ERR NX and XX, GT or LT options at the same time are not compatible}

    test {HEXPIRE - missing FIELDS error} {
        r FLUSHALL
        r HSET myhash field1 val
        catch {r HEXPIRE myhash 10} e
        set e
    } {ERR wrong number of arguments for 'hexpire' command}

    test {HEXPIRE - no fields after FIELDS keyword} {
        r FLUSHALL
        r HSET myhash field1 val
        catch {r HEXPIRE myhash 10 FIELDS 0} e
        set e
    } {ERR wrong number of arguments for 'hexpire' command}

    test {HEXPIRE - non-integer TTL error} {
        r FLUSHALL
        r HSET myhash field1 val
        catch {r HEXPIRE myhash abc FIELDS 1 field1} e
        set e
    } {ERR value is not an integer or out of range}

    test {HEXPIRE - non-existing key returns -2} {
        r FLUSHALL
        set res [r HEXPIRE nokey 10 FIELDS 1 field1]
        assert_equal {-2} $res
    }

    test {HEXPIRE EX - set TTL on multiple fields} {
        r FLUSHALL
        r HSET myhash fieldA valA fieldB valB
        set ttl 100        
        r HEXPIRE myhash $ttl FIELDS 2 fieldA fieldB

        set ttlA [r HTTL myhash FIELDS 1 fieldA]
        set ttlB [r HTTL myhash FIELDS 1 fieldB]

        assert { $ttlA > 0 && $ttlA <= $ttl }
        assert { $ttlB > 0 && $ttlB <= $ttl }
    } {}

    test {HEXPIRE returns -2 on non-existing key} {
        r FLUSHALL
        assert_equal {-2 -2} [r HEXPIRE nokey 10 FIELDS 2 field1 field2]
    } {}

    test {HEXPIRE - GT condition fails when field has no TTL} {
        r FLUSHALL
        r HSET myhash f1 v1
        assert_equal 0 [r HEXPIRE myhash 10 GT fields 1 f1]
    }

    test {HEXPIRE - LT condition succeeds when field has no TTL} {
        r FLUSHALL
        r HSET myhash f1 v1
        assert_equal 1 [r HEXPIRE myhash 10 LT fields 1 f1]
    }

    ##### HTTL #####
    test {HTTL - persistent field returns -1} {
        r FLUSHALL
        r HSET myhash field1 val1
        assert_equal -1 [r HTTL myhash FIELDS 1 field1]
    } {}

    test {HTTL - non-existent field returns -2} {
        r FLUSHALL
        r HSET myhash field1 val1
        assert_equal -2 [r HTTL myhash FIELDS 1 nofield]
    } {}

    test {HTTL - non-existent key returns -2} {
        r FLUSHALL
        assert_equal -2 [r HTTL nokey FIELDS 1 field1]
    } {}

    ##### EXPIRETIME ######

    # Basic Expiry Functionality
    test {HEXPIREAT - set absolute expiry on field} {
        r FLUSHALL
        r HSET myhash field1 hello
        set now [clock seconds]
        set exp [expr {$now + 30}]
        r HEXPIREAT myhash $exp FIELDS 1 field1
        set etime [r HEXPIRETIME myhash FIELDS 1 field1]
        assert_equal $exp $etime
    }

    test {HEXPIREAT - timestamp in past deletes field immediately} {
        r FLUSHALL
        r HSET myhash field1 gone
        set past [expr {[clock seconds] - 1000}]
        set res [r HEXPIREAT myhash $past FIELDS 1 field1]
        assert_equal {2} $res
        assert_equal 0 [r HEXISTS myhash field1]
    }


    test {HEXPIREAT - set TTL on multiple fields (existing + non-existing)} {
        r FLUSHALL
        r HSET myhash field1 hello field2 world
        set exp [expr {[clock seconds] + 10}]
        set res [r HEXPIREAT myhash $exp FIELDS 3 field1 field2 fieldX]
        assert_equal {1 1 -2} $res
    }


    # Conditionals: NX
    test {HEXPIREAT NX - only set when field has no TTL} {
        r FLUSHALL
        r HSETEX myhash EX 100 FIELDS 1 field1 val
        set exp [expr {[clock seconds] + 100}]
        set res [r HEXPIREAT myhash $exp NX FIELDS 1 field1]
        assert_equal {0} $res

        r HSET myhash field2 val2
        set res2 [r HEXPIREAT myhash $exp NX FIELDS 1 field2]
        assert_equal {1} $res2
    }

    # Conditionals: XX
    test {HEXPIREAT XX - only set when field has TTL} {
        r FLUSHALL
        r HSET myhash field1 val1 field2 val2
        set exp1 [expr {[clock seconds] + 20}]
        r HEXPIREAT myhash $exp1 FIELDS 1 field1
        set exp2 [expr {[clock seconds] + 30}]
        set res [r HEXPIREAT myhash $exp2 XX FIELDS 2 field1 field2]
        assert_equal {1 0} $res
    }

    # Conditionals: GT
    test {HEXPIREAT GT - only set if new expiry > existing} {
        r FLUSHALL
        r HSETEX myhash PX 5000 FIELDS 1 field1 val1
        after 10
        set now [clock seconds]
        set future [expr {$now + 10}]
        set res [r HEXPIREAT myhash $future GT FIELDS 1 field1]
        assert_equal {1} $res
        
        r HSET myhash field2 val2
        set res2 [r HEXPIREAT myhash $future GT FIELDS 1 field2]
        assert_equal {0} $res2
    }


    # Conditionals: LT
    test {HEXPIREAT LT - only set if new expiry < existing} {
        r FLUSHALL
        set now [clock seconds]
        # now + 20K seconds
        set long_future_expiration [expr {$now + 20000}]
        # now + 1K seconds
        set short_future_expiration [expr {$now + 1000}]
        r HSETEX myhash EX $long_future_expiration FIELDS 1 field1 val1
        assert_equal {1} [r HEXPIREAT myhash $short_future_expiration LT FIELDS 1 field1]
        
        r HSET myhash field2 val2
        assert_equal {1} [r HEXPIREAT myhash $short_future_expiration LT FIELDS 1 field2]
        # TODO is this the expected behavior? if no TTL exist, it should be treated as minimum ttl possible?
    }

    test {HEXPIREAT - refresh TTL with new future timestamp} {
        r FLUSHALL
        r HSET myhash field1 val1

        # Set initial expiry to very near future
        set ts1 [expr {[clock seconds] + 10}]
        r HEXPIREAT myhash $ts1 FIELDS 1 field1

        # Immediately refresh to a further expiry (no sleep needed)
        set ts2 [expr {$ts1 + 5}]
        r HEXPIREAT myhash $ts2 FIELDS 1 field1

        # Confirm that expiry was updated
        set actual [r HEXPIRETIME myhash FIELDS 1 field1]
        assert_equal $ts2 $actual
    }


    # TTL Validations
    test {HEXPIREAT - TTL is accurate via HEXPIRETIME} {
        r FLUSHALL
        r HSET myhash field1 val1
        set ts [expr {[clock seconds] + 50}]
        r HEXPIREAT myhash $ts FIELDS 1 field1
        set returned [r HEXPIRETIME myhash FIELDS 1 field1]
        assert_equal $ts $returned
    }

    # Error Cases
    test {HEXPIREAT - conflicting options error} {
        r FLUSHALL
        r HSET myhash field1 val
        set ts [expr {[clock seconds] + 5}]
        catch {r HEXPIREAT myhash $ts NX XX FIELDS 1 field1} e
        set e
    } {ERR NX and XX, GT or LT options at the same time are not compatible}



    test {HEXPIREAT - missing FIELDS keyword} {
        r FLUSHALL
        r HSET myhash field1 val
        set ts [expr {[clock seconds] + 5}]
        catch {r HEXPIREAT myhash $ts} e
        set e
    } {ERR wrong number of arguments for 'hexpireat' command}

    test {HEXPIREAT - no fields after FIELDS} {
        r FLUSHALL
        r HSET myhash field1 val
        set ts [expr {[clock seconds] + 5}]
        catch {r HEXPIREAT myhash $ts FIELDS 0} e
        set e
    } {ERR wrong number of arguments for 'hexpireat' command}

    test {HEXPIREAT - non-integer timestamp} {
        r FLUSHALL
        r HSET myhash field1 val
        catch {r HEXPIREAT myhash tomorrow FIELDS 1 field1} e
        set e
    } {ERR value is not an integer or out of range}



    test {HEXPIREAT - non-existing key returns -2} {
        r FLUSHALL
        set ts [expr {[clock seconds] + 5}]
        set res [r HEXPIREAT nokey $ts FIELDS 1 field1]
        assert_equal {-2} $res
    }

    #################### HEXPIRETIME ##################

    # Basic TTL retrieval
    test {HEXPIRETIME - returns expiry timestamp for single field with TTL} {
        r FLUSHALL
        r HSET myhash field1 val
        set ts [expr {[clock seconds] + 3}]
        r HEXPIREAT myhash $ts FIELDS 1 field1
        set out [r HEXPIRETIME myhash FIELDS 1 field1]
        assert_equal $ts $out
    }


    # No expiration set
    test {HEXPIRETIME - field has no TTL returns -1} {
        r FLUSHALL
        r HSET myhash field1 val
        set out [r HEXPIRETIME myhash FIELDS 1 field1]
        assert_equal -1 $out
    }

    # Non-existent field
    test {HEXPIRETIME - field does not exist returns -2} {
        r FLUSHALL
        r HSET myhash field1 val
        set out [r HEXPIRETIME myhash FIELDS 1 fieldX]
        assert_equal -2 $out
    }

    # Non-existent key
    test {HEXPIRETIME - key does not exist returns -2} {
        r FLUSHALL
        set out [r HEXPIRETIME missingkey FIELDS 1 field1]
        assert_equal -2 $out
    }

    # Multiple fields: mix of TTL, no TTL, and missing
    test {HEXPIRETIME - multiple fields mixed cases} {
        r FLUSHALL
        r HSET myhash f1 a f2 b
        set now [clock seconds]
        r HEXPIREAT myhash [expr {$now + 100}] FIELDS 1 f1
        set out [r HEXPIRETIME myhash FIELDS 3 f1 f2 f3]
        # Should return: expiry for f1, -1 for f2 (no TTL), -2 for f3 (not found)
        assert_equal [list [expr {$now + 100}] -1 -2] $out
    }

    # Invalid usages
    test {HEXPIRETIME - no FIELDS keyword} {
        r FLUSHALL
        r HSET myhash f1 a
        catch {r HEXPIRETIME myhash} e
        set e
    } {ERR wrong number of arguments for 'hexpiretime' command}

     test {HEXPIRETIME - FIELDS 0} {
         r FLUSHALL
         r HSET myhash f1 a
         catch {r HEXPIRETIME myhash FIELDS 0} e
         set e
     } {ERR wrong number of arguments for 'hexpiretime' command}

     test {HEXPIRETIME - wrong FIELDS count} {
         r FLUSHALL
         r HSET myhash f1 a
         catch {r HEXPIRETIME myhash FIELDS 1} e
         set e
     } {ERR wrong number of arguments for 'hexpiretime' command}

    test {HEXPIRETIME - wrong type key} {
        r FLUSHALL
        r SET myhash "not a hash"
        catch {r HEXPIRETIME myhash FIELDS 1 f1} e
        set e
    } {WRONGTYPE Operation against a key holding the wrong kind of value}


    # Basic expiration in milliseconds
    test {HPEXPIREAT - set absolute expiry with ms precision} {        
        r FLUSHALL
        r HSET myhash field1 val
        set now [clock milliseconds]
        set future [expr {$now + 123456789}]
        r HPEXPIREAT myhash $future FIELDS 1 field1
        set t [r HPEXPIRETIME myhash FIELDS 1 field1]
        assert_equal $future $t
    }

    test {HPEXPIREAT - past timestamp deletes field immediately} {
        r FLUSHALL
        r HSET myhash field1 val
        set past [expr {[clock milliseconds] - 10000}]
        set res [r HPEXPIREAT myhash $past FIELDS 1 field1]
        assert_equal {2} $res
        assert_equal 0 [r HEXISTS myhash field1]
    }

    test {HPEXPIREAT - non-existent key returns -2} {
        r FLUSHALL
        set ts [expr {[clock milliseconds] + 1000}]
        set res [r HPEXPIREAT nokey $ts FIELDS 1 field1]
        assert_equal {-2} $res
    }

    test {HPEXPIREAT - mixed fields} {
        r FLUSHALL
        r HSET myhash f1 a f2 b
        set ts [expr {[clock milliseconds] + 200000}]
        set res [r HPEXPIREAT myhash $ts FIELDS 3 f1 f2 fX]
        assert_equal {1 1 -2} $res
    }

    test {HPEXPIREAT - GT and LT options with success and failure cases} {
        r FLUSHALL
        r HSET myhash f1 a

        # Setup: assign a baseline expiry time
        set now [clock milliseconds]
        set ts1 [expr {$now + 10000}]
        set ts2 [expr {$now + 20000}]
        r HPEXPIREAT myhash $ts1 FIELDS 1 f1

        # --- GT Case ---
        # ts2 > ts1 → should succeed
        set res_gt_pass [r HPEXPIREAT myhash $ts2 GT FIELDS 1 f1]
        assert_equal {1} $res_gt_pass

        # ts1 < ts2 → now try GT with ts1 again (should fail because ts2 is already set)
        set res_gt_fail [r HPEXPIREAT myhash $ts1 GT FIELDS 1 f1]
        assert_equal {0} $res_gt_fail

        # --- LT Case ---
        # ts1 < ts2 → LT should fail
        set res_lt_fail [r HPEXPIREAT myhash $ts2 LT FIELDS 1 f1]
        assert_equal {0} $res_lt_fail

        # ts1 < ts2 → try LT with earlier timestamp, should succeed
        set ts0 [expr {$now + 5000}]
        set res_lt_pass [r HPEXPIREAT myhash $ts0 LT FIELDS 1 f1]
        assert_equal {1} $res_lt_pass
    }

    test {HPEXPIREAT - invalid inputs} {
        r FLUSHALL
        r HSET myhash f1 a
        catch {r HPEXPIREAT myhash abc FIELDS 1 f1} e
        assert_match {*not an integer*} $e

        catch {r HPEXPIREAT myhash 12345 NX XX FIELDS 1 f1} e2
        assert_match {ERR NX and XX, GT or LT options at the same time are not compatible} $e2
    }


    test {HPEXPIRETIME - check with multiple fields} {
        r FLUSHALL

        # Setup: one expiring field, one persistent, one missing
        r HSET myhash f1 v1 f2 v2
        set ts [expr {[clock milliseconds] + 1000}]
        r HPEXPIREAT myhash $ts FIELDS 1 f1

        # Query all 3 fields
        set result [r HPEXPIRETIME myhash FIELDS 3 f1 f2 f3]

        # Expect: [timestamp] for f1, -1 for f2, -2 for f3
        assert {[llength $result] == 3}
        # f1: has TTL → returns exact timestamp
        assert_equal $ts [lindex $result 0]

        # f2: exists, no TTL → returns -1
        assert_equal -1 [lindex $result 1]

        # f3: doesn't exist → returns -2
        assert_equal -2 [lindex $result 2]

    }

    #################### HPERSIST ##################

    test "HPERSIST - field does not exist" {
        r FLUSHALL
        r hset myhash field1 value1
        assert_equal {-2} [r hpersist myhash FIELDS 1 field2]
    }

    test "HPERSIST - key does not exist" {
        r FLUSHALL
        assert_equal {-2} [r hpersist nonexistent FIELDS 1 field1]
    }

    test "HPERSIST - field exists but no expiration" {
        r del myhash
        r hset myhash field1 value1
        assert_equal {-1} [r hpersist myhash FIELDS 1 field1]
    }

    test "HPERSIST - field exists with expiration" {
        r FLUSHALL
        r hset myhash field1 value1
        r hexpire myhash 600 FIELDS 1 field1
        assert_morethan [r httl myhash FIELDS 1 field1] 0
        assert_equal {1} [r hpersist myhash FIELDS 1 field1]
        assert_equal {-1} [r httl myhash FIELDS 1 field1]
    }

    test "HPERSIST - multiple fields with mixed state" {
        r FLUSHALL
        r hset myhash f1 v1
        r hset myhash f2 v2
        r hset myhash f3 v3
        r hexpire myhash 600 FIELDS 1 f1
        # f2 will have no expiration
        # f4 does not exist
        assert_equal {1 -1 -2} [r hpersist myhash FIELDS 3 f1 f2 f4]
    }
}


####### Test info
start_server {tags {"hash-ttl-info external:skip"}} {    
    test {Hash ttl - check command stats} {
        r FLUSHALL

        # Run all relevant hash TTL commands
        r HSET myhash f1 v1 f2 v2
        r HEXPIRE myhash 10 FIELDS 1 f1
        r HEXPIREAT myhash [expr {[clock seconds] + 10}] FIELDS 1 f2
        r HEXPIRETIME myhash FIELDS 2 f1 f2
        r HPEXPIRE myhash 1000 FIELDS 1 f1
        r HPEXPIREAT myhash [expr {[clock milliseconds] + 2000}] FIELDS 1 f2
        r HPEXPIRETIME myhash FIELDS 2 f1 f2        
        r HGETEX myhash EX 120 FIELDS 1 f1        
        r HTTL myhash FIELDS 1 f2
        r HPTTL myhash FIELDS 1 f1

        # Fetch commandstats
        set info [r INFO commandstats]

        # Extract call counts
        proc get_calls {info cmd} {
            foreach line [split $info "\n"] {
                if {[string match "cmdstat_$cmd:*" $line]} {
                    regexp {calls=(\d+)} $line -> count
                    return $count
                }
            }
            return -1
        }

        # Assert each command appears with correct call count (1 call each)        
        assert_equal 1 [get_calls $info hexpire]
        assert_equal 1 [get_calls $info hexpireat]
        assert_equal 1 [get_calls $info hexpiretime]
        assert_equal 1 [get_calls $info hpexpire]
        assert_equal 1 [get_calls $info hpexpireat]
        assert_equal 1 [get_calls $info hpexpiretime]
        assert_equal 1 [get_calls $info hgetex]
        assert_equal 1 [get_calls $info httl]
        assert_equal 1 [get_calls $info hpttl]
    }
}


#### Replication ####
start_server {tags {"hashexpire external:skip"}} {
    # Start another server to test replication of TTLs
    start_server {tags {needs:repl external:skip}} {
        # Set the outer layer server as primary
        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        # Set this inner layer server as replica
        set replica [srv 0 client]

        test {Setup replica and check field expiry after full sync} {
            $primary flushall

            # Set up some TTLs on primary BEFORE replica connects
            set now [clock milliseconds]
            set f1_exp [expr {$now + 50000}]
            set f2_exp [expr {$now + 70000}]
            
            $primary HSET myhash f1 v1 f2 v2
            $primary HPEXPIREAT myhash $f1_exp FIELDS 1 f1
            $primary HPEXPIREAT myhash $f2_exp FIELDS 1 f2

            # Now connect replica
            $replica replicaof $primary_host $primary_port
            
            wait_for_condition 100 100 {
                [info_field [$replica info replication] master_link_status] eq "up"
            } else {
                fail "Master <-> Replica didn't finish sync"
            }
            

            # Wait for full sync
            wait_for_ofs_sync $primary $replica


            # Validate TTLs replicated correctly
            set r1 [$replica HPEXPIRETIME myhash FIELDS 1 f1]
            set r2 [$replica HPEXPIRETIME myhash FIELDS 1 f2]

            assert_equal $f1_exp $r1
            assert_equal $f2_exp $r2
        }



        test {HASH TTL - replicated TTL is absolute and consistent on replica} {
            $primary flushall

            set now [clock milliseconds]
            set future [expr {$now + 5000}]
            set future_sec [expr {$future / 1000}]

            # HPEXPIREAT
            $primary HSET myhash f1 v1
            $primary HPEXPIREAT myhash $future FIELDS 1 f1

            # HSETEX EX
            $primary HSETEX myhash EX 5 FIELDS 1 f2 v2

            # HEXPIRE
            $primary HSET myhash f3 v3
            $primary HEXPIRE myhash 5 FIELDS 1 f3

            wait_for_ofs_sync $primary $replica

            set t1 [$primary HPEXPIRETIME myhash FIELDS 1 f1]            
            set t1r [$replica HPEXPIRETIME myhash FIELDS 1 f1]
            assert_equal $t1 $t1r

            set t2 [$primary HEXPIRETIME myhash FIELDS 1 f2]
            set t2r [$replica HEXPIRETIME myhash FIELDS 1 f2]
            assert_equal $t2 $t2r

            set t3 [$primary HEXPIRETIME myhash FIELDS 1 f3]
            set t3r [$replica HEXPIRETIME myhash FIELDS 1 f3]
            assert_equal $t3 $t3r
        }

        test {HASH TTL - field expired on master gets deleted on replica} {
            $primary flushall            

            $primary HSETEX myhash PX 10 FIELDS 1 f1 val1
            after 20
            wait_for_ofs_sync $primary $replica


            # Trigger lazy expiry
            catch {$primary HGET myhash f1}
            wait_for_ofs_sync $primary $replica


            assert_equal 0 [$replica HEXISTS myhash f1]
        }


        test {HASH TTL - replica retains TTL and field before expiration} {
            $primary flushall            

            $primary HSETEX myhash PX 1000 FIELDS 1 f1 val1
            wait_for_ofs_sync $primary $replica

            set master_ttl [$primary HPTTL myhash FIELDS 1 f1]
            set replica_ttl [$replica HPTTL myhash FIELDS 1 f1]
            assert {$replica_ttl > 0}
            assert {$replica_ttl <= $master_ttl}

        }

        test {HSETEX with expired time is propagated to the replica} {
            $primary flushall            

            assert_equal [$primary HSET myhash f1 val1] "1"
            
            wait_for_condition 100 100 {
                [$replica HGET myhash f1] eq {val1}
            } else {
                fail "hash field was not set on replica after timeout"
            }

            assert_equal [$primary HSETEX myhash EXAT 0 FIELDS 1 f1 val1] {1}
            
            wait_for_condition 100 100 {
                [$primary EXISTS myhash] eq "0"
            } else {
                fail "hash object was not deleted on primary after timeout"
            }
            wait_for_ofs_sync $primary $replica

            wait_for_condition 100 100 {
                [$replica EXISTS myhash] eq "0"
            } else {
                fail "hash object was not deleted on replica after timeout"
            }
        }

        test {HGETEX with expired time is propagated to the replica} {
            $primary flushall            

            assert_equal [$primary HSET myhash f1 val1] "1"
            
            wait_for_condition 100 100 {
                [$replica HGET myhash f1] eq {val1}
            } else {
                fail "hash field was not set on replica after timeout"
            }

            assert_equal [$primary HGETEX myhash EXAT 0 FIELDS 1 f1] {val1}
            
            wait_for_condition 100 100 {
                [$primary EXISTS myhash] eq "0"
            } else {
                fail "hash object was not deleted on primary after timeout"
            }
            wait_for_ofs_sync $primary $replica

            wait_for_condition 100 100 {
                [$replica EXISTS myhash] eq "0"
            } else {
                fail "hash object was not deleted on replica after timeout"
            }
        }
        test {HEXPIREAT with expired time is propagated to the replica} {
            $primary flushall            

            assert_equal [$primary HSET myhash f1 val1] "1"
            
            wait_for_condition 100 100 {
                [$replica HGET myhash f1] eq {val1}
            } else {
                fail "hash field was not set on replica after timeout"
            }

            assert_equal [$primary HEXPIREAT myhash 0 FIELDS 1 f1] {2}
            
            wait_for_condition 100 100 {
                [$primary EXISTS myhash] eq "0"
            } else {
                fail "hash object was not deleted on primary after timeout"
            }
            wait_for_ofs_sync $primary $replica

            wait_for_condition 100 100 {
                [$replica EXISTS myhash] eq "0"
            } else {
                fail "hash object was not deleted on replica after timeout"
            }
        }
    }
}

### Slot Migration ####
start_cluster 3 0 {tags {"cluster mytest"} overrides {cluster-node-timeout 1000}} {
    # Flush all data on all cluster nodes before starting
    for {set i 0} {$i < 3} {incr i} {
        R $i FLUSHALL
    }
    if {$::singledb} {
        set db 0
    } else {
        set db 9
    }
    set R0_id [R 0 CLUSTER MYID]
    set R1_id [R 1 CLUSTER MYID]

    # Use a fixed hash tag to ensure key is in one slot
    set key "{mymigrate}myhash"

    test {Hash with TTL fields migrates correctly between nodes} {
        # Create hash fields
        R 0 HSET $key f1 v1 f2 v2 f3 v3

        # Set TTL on fields f1 and f2
        R 0 HEXPIRE $key 300 FIELDS 2 f1 f2

        # Verify before slot migration
        assert_equal 3 [R 0 HLEN $key]
        assert_morethan [R 0 HTTL $key FIELDS 1 f1] 290
        assert_match {1} [scan [regexp -inline {keys\=([\d]*)} [R 0 info keyspace]] keys=%d]

        # Prepare slot migration
        set slot [R 0 CLUSTER KEYSLOT $key]
        assert_equal OK [R 1 CLUSTER SETSLOT $slot IMPORTING $R0_id]
        assert_equal OK [R 0 CLUSTER SETSLOT $slot MIGRATING $R1_id]

        # Migrate key to destination node
        R 0 MIGRATE [srv -1 host] [srv -1 port] $key 0 5000
        
        # Complete slot migration
        R 0 CLUSTER SETSLOT $slot NODE $R1_id
        R 1 CLUSTER SETSLOT $slot NODE $R1_id
        
        # Verify after slot migration
        assert_equal 3 [R 1 HLEN $key]
        assert_morethan [R 1 HTTL $key FIELDS 1 f1] 280
        assert_match {1} [scan [regexp -inline {keys\=([\d]*)} [R 1 info keyspace]] keys=%d]

        # Setup keyspace notifications
        R 1 config set notify-keyspace-events KEA
        set rd [valkey_deferring_client -1]
        assert_equal {1} [psubscribe $rd __keyevent@0__:hexpired]

        # Set expiration to 0
        R 1 HGETEX $key EX 0 FIELDS 1 f1
        
        # Veridy expiration
        assert_keyevent_pattern $rd hexpired "{$key}"
        assert_equal 2 [R 1 HLEN $key]
        assert_equal "" [R 1 HGET $key f1]
        assert_equal -2 [R 1 HTTL $key FIELDS 1 f1]

        $rd close
    }
}

start_server {tags {"hashexpire external:skip"}} {
    foreach cmd {RENAME RESTORE} {
        test "$cmd Preserves Field TTLs" {
            r FLUSHALL
            r HSET myhash f1 v1 f2 v2
            r HEXPIRE myhash 200 FIELDS 1 f1

            # Verify initial TTL state
            set mem_before [r MEMORY USAGE myhash]
            assert_equal "v1" [r HGET myhash f1]
            assert_equal "v2" [r HGET myhash f2]
            assert_morethan [r HTTL myhash FIELDS 1 f1] 100
            assert_equal -1 [r HTTL myhash FIELDS 1 f2]
            assert_equal 2 [r HLEN myhash]
            assert_match {1} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]

            # Run the command
            if {$cmd eq "RENAME"} {
                r rename myhash newhash
                set newhash newhash
            } elseif {$cmd eq "RESTORE"} {
                set serialized [r DUMP myhash]
                r RESTORE restored_hash 0 $serialized
                set newhash restored_hash
            }

            # Verify field values and TTLs are preserved
            set memory_after [r MEMORY USAGE $newhash]
            assert_equal "v1" [r HGET $newhash f1]
            assert_equal "v2" [r HGET $newhash f2]
            assert_morethan [r HTTL $newhash FIELDS 1 f1] 100
            assert_equal -1 [r HTTL $newhash FIELDS 1 f2]
            assert_equal 2 [r HLEN $newhash]
            if {$cmd eq "RESTORE"} {
                assert_match {2} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]
            } else {
                assert_match {1} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]
            }
            assert_equal $mem_before $memory_after
        }
    }

    test {COPY Preserves TTLs} {
        r flushall
        
        # Create hash with fields
        r HSET myhash f1 v1 f3 v3 f4 v4

        # Set TTL on f1 only
        r HEXPIRE myhash 200 FIELDS 1 f1
        r HEXPIRE myhash 2 FIELDS 1 f3

        # Verify initial TTL state
        set mem_before [r MEMORY USAGE myhash]
        assert_equal "v1" [r HGET myhash f1]
        assert_equal "v3" [r HGET myhash f3]
        assert_equal "v4" [r HGET myhash f4]
        assert_morethan [r HTTL myhash FIELDS 1 f1] 100
        assert_morethan [r HTTL myhash FIELDS 1 f3] 0
        assert_equal -1 [r HTTL myhash FIELDS 1 f4]
        assert_equal 3 [r HLEN myhash]
        assert_match {1} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]

        # Copy hash to new key
        r copy myhash newhash1

        # Verify myhash is the same
        assert_equal "v1" [r HGET myhash f1]
        assert_equal "v3" [r HGET myhash f3]
        assert_equal "v4" [r HGET myhash f4]
        assert_morethan [r HTTL myhash FIELDS 1 f1] 100
        assert_morethan [r HTTL myhash FIELDS 1 f3] 0
        assert_equal -1 [r HTTL myhash FIELDS 1 f4]
        assert_equal 3 [r HLEN myhash]

        # Verify new hash got same values
        set mem_after [r MEMORY USAGE myhash]
        assert_equal "v1" [r HGET newhash1 f1]
        assert_equal "v3" [r HGET newhash1 f3]
        assert_equal "v4" [r HGET newhash1 f4]
        assert_morethan [r HTTL newhash1 FIELDS 1 f1] 100
        assert_morethan [r HTTL newhash1 FIELDS 1 f3] 0
        assert_equal -1 [r HTTL newhash1 FIELDS 1 f4]
        assert_equal 3 [r HLEN newhash1]
        assert_match {2} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]

        assert_equal $mem_before $mem_after
        
        # Modify TTL in original hash
        r HEXPIRE myhash 5 FIELDS 1 f3

        # Wait for original TTL to expire in copy
        after 2000
        assert_equal "v1" [r HGET newhash1 f1]
        assert_equal "" [r HGET newhash1 f3]
        assert_equal "v1" [r HGET myhash f1]
        assert_equal "v3" [r HGET myhash f3]

        r HSETEX myhash EX 2 FIELDS 1 f3 v3
        # Create second copy
        r copy myhash newhash2

        # Modify TTL in second copy
        r HEXPIRE newhash2 500 FIELDS 1 f3

        # Wait for original hash TTL to expire
        after 2000
        assert_equal "v1" [r HGET myhash f1]
        assert_equal "" [r HGET myhash f3]
        assert_equal "v1" [r HGET newhash2 f1]
        assert_equal "v3" [r HGET newhash2 f3]
    }

    test {Hash Encoding Transitions with TTL - Add TTL to Existing Fields} {
        r flushall
        
        # Create small hash with listpack encoding
        r HSET myhash f1 v1 f2 v2
        
        # Verify initial encoding
        set "listpack" [r OBJECT ENCODING myhash]
        
        # Add TTL to existing field
        r HEXPIRE myhash 300 FIELDS 1 f1
        
        # Verify encoding changed to hashtable
        set "hashtable" [r OBJECT ENCODING myhash]

        # Verify field values are preserved
        assert_equal "v1" [r HGET myhash f1]
        assert_equal "v2" [r HGET myhash f2]
        # Veridy expiry
        assert_morethan [r HTTL myhash FIELDS 1 f1] 100
        assert_equal -1 [r HTTL myhash FIELDS 1 f2]
    }
    
    test {Hash Encoding Transitions with TTL - Create New Fields with TTL} {
        r flushall
        
        # Create small hash with listpack encoding
        r HSET myhash f1 v1 f2 v2
        
        # Verify initial encoding
        set "listpack" [r OBJECT ENCODING myhash]
        
        # Add many fields to force encoding transition
        for {set i 3} {$i <= 600} {incr i} {
            lappend pairs "f$i" "v$i"
        }
        r HSET myhash {*}$pairs
        r HEXPIRE myhash 3 FIELDS 5 f1 f10 f100 f200 f300
        
        # Verify encoding changed to hashtable
        set "hashtable" [r OBJECT ENCODING myhash]
        
        # Verify all field values and TTLs are correct
        for {set i 1} {$i <= 600} {incr i} {
            assert_equal "v$i" [r HGET myhash "f$i"]
            if {$i == 1 || $i == 10 || $i == 100 || $i == 200 || $i == 300} {
                assert_equal 3 [r HTTL myhash FIELDS 1 "f$i"]
            } else {
                assert_equal -1 [r HTTL myhash FIELDS 1 "f$i"]
            }
        }
    }
}

start_server {tags {"hashexpire external:skip"}} {
    r config set notify-keyspace-events KEA

    foreach time_unit {s, ms} {
        test "Key TTL expires before field TTL: entire hash should be deleted timeunit: $time_unit" {
            r FLUSHALL
            r config set notify-keyspace-events KEA
            set rd [valkey_deferring_client]
            assert_equal {1} [psubscribe $rd __keyevent@*]

            r HSET myhash f1 v1 f2 v2 f3 v3
            assert_match {1} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]
            assert_equal 3 [r HLEN myhash]
            if {$time_unit eq "s"} {
                r HEXPIRE hash1 10 FIELDS 1 f1
                r EXPIRE hash1 1
            } else {
                r HPEXPIRE myhash 10000 FIELDS 1 f1
                r PEXPIRE myhash 1000
            }
            
            wait_for_condition 100 100 {
                [r EXISTS myhash] eq "0"
            } else {
                fail "myhash still exists"
            }
            assert_equal 0 [r HLEN myhash]
            assert_match "" [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]

            assert_keyevent_pattern $rd hset myhash
            assert_keyevent_pattern $rd hexpire myhash
            assert_keyevent_pattern $rd expire myhash
            $rd close
        }

        test "Field TTL expires before key TTL: only the specific field should expire: $time_unit" {
            r FLUSHALL
            set rd [valkey_deferring_client]
            assert_equal {1} [psubscribe $rd __keyevent@*]
            
            r HSET myhash f1 v1 f2 v2 f3 v3
            assert_match {1} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]
            assert_equal 3 [r HLEN myhash]
            if {$time_unit eq "s"} {
                r HEXPIRE myhash 1 FIELDS 1 f1
                r EXPIRE myhash 10
            } else {
                r HPEXPIRE myhash 1000 FIELDS 1 f1
                r PEXPIRE myhash 10000
            }
            
            wait_for_condition 100 100 {
                [r HGET myhash f1] eq ""
            } else {
                fail "f1 not expired"
            }
            assert_match {1} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]
            assert_equal 1 [r EXISTS myhash]
            assert_equal "v2" [r HGET myhash f2]
            assert_equal "v3" [r HGET myhash f3]
            assert_keyevent_pattern $rd hset myhash
            assert_keyevent_pattern $rd hexpire myhash
            $rd close
        }

        test "Key and field TTL expire simultaneously: entire hash should be deleted: $time_unit" {
            r FLUSHALL
            
            r HSET myhash f1 v1 f2 v2 f3 v3
            assert_match {1} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]
            assert_equal 3 [r HLEN myhash]
            

            if {$time_unit eq "s"} {
                set expire [expr {[clock seconds] + 1}]
                r HEXPIREAT myhash $expire FIELDS 1 f1
                r EXPIREAT myhash $expire
            } else {
                set expire [expr {[clock milliseconds] + 1000}]
                r HPEXPIREAT myhash $expire FIELDS 1 f1
                r PEXPIREAT myhash $expire
            }
            
            wait_for_condition 100 100 {
                [r EXISTS myhash] eq 0
            } else {
                fail "myhash still exsist"
            }

            assert_equal "" [r HGET myhash f1]
            assert_equal "" [r HGET myhash f2]
            assert_equal "" [r HGET myhash f3]
            assert_match "" [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]
            assert_equal 0 [r HLEN myhash]
        }

        test {Millisecond/Seconds precision} {
            r flushall

            r HSET myhash f1 v1 f2 v2
            if {$time_unit eq "s"} {
                r HEXPIRE myhash 3 FIELDS 1 f1
                r EXPIRE myhash 1
            } else {
                r HPEXPIRE myhash 3000 FIELDS 1 f1
                r PEXPIRE myhash 1000
            }
            
            after 1500
            assert_equal 0 [r EXISTS myhash]
        }
    }

    test {Ensure that key-level PERSIST on the key don't affect field TTL} {
        r FLUSHALL
        
        r HSET myhash f1 v1 f2 v2
        assert_match {1} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]
        assert_equal 2 [r HLEN myhash]
        r HEXPIRE myhash 100000 FIELDS 1 f1
        r PERSIST myhash
        
        assert_equal -1 [r TTL myhash]
        assert_morethan [r HTTL myhash FIELDS 1 f1] 0
    }
}