proc info_field {info field} {
    foreach line [split $info "\n"] {
        if {[string match "$field:*" $line]} {
            return [string trim [lindex [split $line ":"] 1]]
        }
    }
    return [s field_name]
}

proc get_keys_with_volatile_items {r} {
    set line [$r info keyspace]
    set match [regexp -inline {keys_with_volatile_items=([\d]+)} $line]

    if {[llength $match] == 2} {
        return [lindex $match 1]
    } else {
        return 0
    }
}

proc get_keys {r} {
    set line [$r info keyspace]
    set match [regexp -inline {keys=([\d]+)} $line]

    if {[llength $match] == 2} {
        return [lindex $match 1]
    } else {
        return 0
    }
}

proc check_myhash_and_expired_subkeys {r myhash expected_len initial_expired expected_increment} {
    expr {
        [$r HLEN $myhash] == $expected_len &&
        [info_field [$r info stats] expired_fields] == ($initial_expired + $expected_increment)
    }
}

proc get_short_expire_value {command} {
    expr {
        ($command eq "HEXPIRE" || $command eq "EX") ? 1 :
        ($command eq "HPEXPIRE" || $command eq "PX") ? 1000 :
        ($command eq "HEXPIREAT" || $command eq "EXAT") ? [clock seconds] + 1 :
        [clock milliseconds] + 1000
    }
}

proc get_long_expire_value {command} {
    expr {
        ($command eq "HEXPIRE" || $command eq "EX") ? 60000000 :
        ($command eq "HPEXPIRE" || $command eq "PX") ? 60000000 :
        ($command eq "HEXPIREAT" || $command eq "EXAT") ? [clock seconds] + 60000000 :
        [clock milliseconds] + 60000000
    }
}

proc get_longer_then_long_expire_value {command} {
    expr {
        ($command eq "HEXPIRE" || $command eq "EX") ? 1200000000 :
        ($command eq "HPEXPIRE" || $command eq "PX") ? 1200000000 :
        ($command eq "HEXPIREAT" || $command eq "EXAT") ? [clock seconds] + 1200000000 :
        [clock milliseconds] + 1200000000
    }
}

proc get_past_zero_expire_value {command} {
    expr {
        ($command eq "HEXPIRE" || $command eq "EX") ? 0 :
        ($command eq "HPEXPIRE" || $command eq "PX") ? 0 :
        ($command eq "HEXPIREAT" || $command eq "EXAT") ? [clock seconds] - 200000 :
        [clock milliseconds] - 200000
    }
}

proc get_check_ttl_command {command} {
    if {$command eq "EX"} {
        return "HTTL"
    } elseif {$command eq "PX"} {
        return "HPTTL"
    } elseif {$command eq "EXAT"} {
        return "HEXPIRETIME"
    } else {
        return "HPEXPIRETIME"
    }
}

proc assert_keyevent_patterns {rd key args} {
    foreach event_type $args {
        set event [$rd read]
        assert_match "pmessage __keyevent@* __keyevent@*:$event_type $key" $event
    }
}

proc setup_replication_test {primary replica primary_host primary_port} {
    $primary FLUSHALL
    $replica replicaof $primary_host $primary_port
    wait_for_condition 50 100 {
        [lindex [$replica role] 0] eq {slave} &&
        [string match {*master_link_status:up*} [$replica info replication]]
    } else {
        fail "Can't turn the instance into a replica"
    }
    set primary_initial_expired [info_field [$primary info stats] expired_fields]
    set replica_initial_expired [info_field [$replica info stats] expired_fields]
    return [list $primary_initial_expired $replica_initial_expired]
}

proc setup_single_keyspace_notification {r} {
    $r config set notify-keyspace-events KEA
    set rd [valkey_deferring_client]
    assert_equal {1} [psubscribe $rd __keyevent@*]
    return $rd
}

proc wait_for_active_expiry {r key expected_len initial_expired expected_increment {timeout 100} {interval 100}} {
    wait_for_condition $timeout $interval {
        [check_myhash_and_expired_subkeys $r $key $expected_len $initial_expired $expected_increment]
    } else {
        fail "Active expiry did not occur as expected"
    }
}

start_server {tags {"hashexpire"}} {
    ####### Valid scenarios tests #######
    foreach command {EX PX EXAT PXAT} {
        test "HGETEX $command expiry" {
            r FLUSHALL
            r DEBUG SET-ACTIVE-EXPIRE 0
            r HSET myhash f1 v1
            
            set ttl_cmd [get_check_ttl_command $command]
            set expire_time [get_long_expire_value $command]
            
            # Verify HGETEX command
            assert_equal "v1" [r HGETEX myhash $command $expire_time FIELDS 1 f1]
            set expire_result [r $ttl_cmd myhash FIELDS 1 f1]
            
            # Verify expiry
            if {[regexp "AT$" $command]} {
                assert_equal $expire_result $expire_time
            } else {
                assert_morethan $expire_result 0
            }
            # Re-enable active expiry
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "HGETEX $command with mix of existing and non-existing fields" {
            r FLUSHALL
            r HSET myhash f1 v1 f3 v3
            
            # HGETEX on exist/non-exist fields
            assert_equal "v1 {} v3" [r HGETEX myhash $command [get_long_expire_value $command] FIELDS 3 f1 f2 f3]
            
            # Verification checks (f2 should not be created)
            assert_equal "" [r HGET myhash f2]
            assert_equal -2 [r HTTL myhash FIELDS 1 f2]
            assert_morethan [r HTTL myhash FIELDS 1 f1] 0
            assert_morethan [r HTTL myhash FIELDS 1 f3] 0
        }

        test "HGETEX $command on more then 1 field" {
            r FLUSHALL
            r DEBUG SET-ACTIVE-EXPIRE 0
            r HSET myhash f1 v1 f2 v2
            
            set ttl_cmd [get_check_ttl_command $command]
            set expire_time [get_long_expire_value $command]
            
            assert_equal "v1 v2" [r HGETEX myhash $command $expire_time FIELDS 2 f1 f2]
            
            # Verify expiration
            if {[regexp "AT$" $command]} {
                assert_equal $expire_time [r $ttl_cmd myhash FIELDS 1 f1]
                assert_equal $expire_time [r $ttl_cmd myhash FIELDS 1 f2]
            } else {
                assert_morethan [r $ttl_cmd myhash FIELDS 1 f1] 0
                assert_morethan [r $ttl_cmd myhash FIELDS 1 f2] 0
            }
            
            # Re-enable active expiry
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "HGETEX $command -> PERSIST" {
            r FLUSHALL
            r HSET myhash f1 v1
            r HSETEX myhash EX 10000 FIELDS 1 f2 v2

            set ttl_cmd [get_check_ttl_command $command]
            set expire_time [get_long_expire_value $command]
            
            assert_equal "v1" [r HGETEX myhash $command $expire_time FIELDS 1 f1]
            if {[regexp "AT$" $command]} {
                assert_equal $expire_time [r $ttl_cmd myhash FIELDS 1 f1]
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
            assert_equal {{}} [r HGETEX myhash $command [get_short_expire_value $command] FIELDS 1 f2]
        }

        test "HGETEX $command on non-exist key" {
            r FLUSHALL
            assert_equal {{} {} {}} [r HGETEX myhash $command [get_long_expire_value $command] FIELDS 3 f1 f2 f3]
        }

        test "HGETEX $command with duplicate field names" {
            r FLUSHALL
            r HSET myhash f1 v1
            assert_equal "v1 v1" [r HGETEX myhash $command [get_long_expire_value $command] FIELDS 2 f1 f1]
        }


        test "HGETEX $command overwrites existing field TTL with bigger value" {
            r FLUSHALL
            r HSETEX myhash $command [get_long_expire_value $command] FIELDS 1 f1 v1
            set old_ttl [r HTTL myhash FIELDS 1 f1]
            r HGETEX myhash $command [get_longer_then_long_expire_value $command] FIELDS 1 f1
            set new_ttl [r HTTL myhash FIELDS 1 f1]
            assert {$new_ttl > $old_ttl}
        }
        
        test "HGETEX $command overwrites existing field TTL with smaller value" {
            r FLUSHALL
            r HSETEX myhash $command [get_long_expire_value $command] FIELDS 1 f1 v1
            set old_ttl [r HTTL myhash FIELDS 1 f1]
            r HGETEX myhash $command [get_short_expire_value $command] FIELDS 1 f1
            set new_ttl [r HTTL myhash FIELDS 1 f1]
            assert {$new_ttl <= $old_ttl}
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
            r FLUSHALL
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
        assert_error "ERR*" {r HGETEX myhash EX 60 PX 1000 FIELDS 1 f1}
    }
    
    test {HGETEX EXAT- multiple options used (EXAT + PXAT)} {
        r FLUSHALL
        r HSET myhash f1 v1
        assert_error "ERR*" {r HGETEX myhash EXAT [expr {[clock seconds] + 100}] PXAT [expr {[clock milliseconds] + 100000}] 1000 FIELDS 1 f1}
    }
    
    # Common error scenarios for all commands
    foreach cmd {EX PX EXAT PXAT} {
        test "HGETEX $cmd- missing TTL value" {
            r FLUSHALL
            r HSET myhash f1 v1
            catch {r HGETEX myhash $cmd FIELDS 1 f1} e
            set e
        } {ERR *}
        
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
            catch {r HGETEX myhash $cmd [get_short_expire_value $cmd] 1 f1} e
            set e
        } {ERR *}
        
        test "HGETEX $cmd- wrong numfields count (too few fields)" {
            r FLUSHALL
            r HSET myhash f1 v1 f2 v2
            catch {r HGETEX myhash $cmd [get_short_expire_value $cmd] FIELDS 2 f1} e
            set e
        } {ERR *}
        
        test "HGETEX $cmd- wrong numfields count (too many fields)" {
            r FLUSHALL
            r HSET myhash f1 v1
            catch {r HGETEX myhash $cmd [get_short_expire_value $cmd] FIELDS 1 f1 f2} e
            set e
        } {ERR *}
        
        test "HGETEX $cmd- key is wrong type (string instead of hash)" {
            r FLUSHALL
            r SET mystring "v1"
            catch {r HGETEX mystring $cmd [get_short_expire_value $cmd] FIELDS 1 f1} e
            set e
        } {WRONGTYPE Operation against a key holding the wrong kind of value}
        
        test "HGETEX $cmd with FIELDS 0" {
            r FLUSHALL
            catch {r HGETEX myhash $cmd [get_short_expire_value $cmd] FIELDS 0} e
            set e
        } {ERR *}
        
        test "HGETEX $cmd with negative numfields" {
            r FLUSHALL
            catch {r HGETEX myhash $cmd [get_short_expire_value $cmd] FIELDS -10} e
            set e
        } {ERR *}

        test "HGETEX $cmd with missing key" {
            r FLUSHALL
            catch {r HGETEX $cmd [get_short_expire_value $cmd] FIELDS 1 f1} e
            set e
        } {ERR *}
    }
}

start_server {tags {"hashexpire"}} {
    if {$::singledb} {
        set db 0
    } else {
        set db 9
    }
    set all_h_pattern "h*"
    set hexpire_pattern "hexpire"
    set hpersist_pattern "hpersist"

    r config set notify-keyspace-events KEA

    ## HGETEX -> Keyspace notification tests ####
    foreach command {EX PX EXAT PXAT} {
        test "HGETEX $command generates hexpire keyspace notification" {
            r FLUSHALL
            r HSET myhash f1 v1
            assert_equal 0 [get_keys_with_volatile_items r]
            set rd [setup_single_keyspace_notification r]
            
            r HGETEX myhash $command [get_long_expire_value $command] FIELDS 1 f1

            assert_keyevent_patterns $rd myhash hexpire
            assert_equal 1 [get_keys_with_volatile_items r]
            $rd close
        }

        test "HGETEX $command with multiple fields generates single notification" {
            r FLUSHALL
            r HSET myhash f1 v1 f2 v2 f3 v3
            assert_equal 0 [get_keys_with_volatile_items r]
            set rd [setup_single_keyspace_notification r]

            r HGETEX myhash $command [get_long_expire_value $command] FIELDS 3 f1 f2 f3
            
            assert_keyevent_patterns $rd myhash hexpire
            # Verify no notification (getting hset and not hexpire)
            r HSET dummy dummy dummy
            assert_keyevent_patterns $rd dummy hset
            assert_equal 1 [get_keys_with_volatile_items r]
            $rd close
        }

        test "HGETEX $command on non-existent field generates no notification" {
            r FLUSHALL
            r HSET myhash f1 v1
            assert_equal 0 [get_keys_with_volatile_items r]
            set rd [setup_single_keyspace_notification r]

            # This HGETEX targets a non-existent field, so no notification about hexpire should be emitted
            r HGETEX myhash $command [get_long_expire_value $command] FIELDS 1 f2
            
            # Verify no notification (getting hset and not hexpire)
            r HSET dummy dummy dummy
            assert_keyevent_patterns $rd dummy hset
            assert_equal 0 [get_keys_with_volatile_items r]
            $rd close
        }
    }
    
    test {HGETEX PERSIST generates hpersist keyspace notification} {
        r FLUSHALL
        r HSET myhash f1 v1
        assert_equal 0 [get_keys_with_volatile_items r]
        
        r HEXPIRE myhash [get_long_expire_value HEXPIRE] FIELDS 1 f1
        assert_equal 1 [get_keys_with_volatile_items r]
        
        set rd [setup_single_keyspace_notification r]
        
        r HGETEX myhash PERSIST FIELDS 1 f1

        assert_keyevent_patterns $rd myhash hpersist
        assert_equal 0 [get_keys_with_volatile_items r]
        $rd close
    }

    foreach command {EX PX EXAT PXAT} {
        test "HGETEX $command 0/past time works correctly with 1 field" {
            r FLUSHALL
            r config resetstat
            # Create hash with field
            r HSET myhash f1 v1
            assert_equal 1 [r HLEN myhash]
            assert_equal 0 [get_keys_with_volatile_items r]
            assert_equal 1 [get_keys r]
            set rd [setup_single_keyspace_notification r]
            
            # Set field to expire immediately
            r HGETEX myhash $command [get_past_zero_expire_value $command] FIELDS 1 f1

            # Verify field and keys are deleted
            assert_keyevent_patterns $rd myhash hexpired del
            assert_equal -2 [r HTTL myhash FIELDS 1 f1]
            assert_equal 0 [r HLEN myhash]
            assert_equal 0 [r EXISTS myhash]
            assert_equal 0 [get_keys r]
            assert_equal 0 [get_keys_with_volatile_items r]
            assert_equal 1 [info_field [r info stats] expired_fields]
            $rd close
        }
    }

    # HSETEX ####
    test {HSETEX KEEPTTL - preserves existing TTL of field} {
        r FLUSHALL

        # Set a field with a known TTL
        r HSETEX myhash PX 1000 FIELDS 1 field1 val1
        set original_pttl [r HPTTL myhash FIELDS 1 field1]
        set original_expiretime [r HEXPIRETIME myhash FIELDS 1 field1]
        assert_equal 1 [get_keys_with_volatile_items r]

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

    test {HSETEX EX - test mix of expiring and persistent fields} {
        r FLUSHALL
        r HSET myhash field2 "persistent"
        r HSETEX myhash EX 1 FIELDS 1 field1 "temp"
        assert_equal 1 [get_keys_with_volatile_items r]
        after 1100
        assert_equal 0 [r HEXISTS myhash field1]
        assert_equal 1 [r HEXISTS myhash field2]
    }

    test {HSETEX EX - test missing TTL} {
        catch {r HSETEX myhash EX FIELDS 1 field1 val1} e
        set e
    } {ERR *}

    test {HSETEX EX - mismatched field/value count} {
        catch {r HSETEX myhash EX 10 FIELDS 2 field1 val1} e
        set e
    } {ERR *}

    foreach command {EX PX EXAT PXAT} {
        test "HSETEX $command 0/past time works correctly with 1 field" {
            r FLUSHALL
            r config resetstat
            # Create hash with field
            r HSET myhash f1 v1
            assert_equal 1 [r HLEN myhash]
            assert_equal 0 [get_keys_with_volatile_items r]
            assert_equal 1 [get_keys r]
            set rd [setup_single_keyspace_notification r]
            
            # Set field to expire immediately
            assert_equal {1} [r HSETEX myhash $command [get_past_zero_expire_value $command] FIELDS 1 f1 v1]

            # Verify field and keys are deleted
            assert_keyevent_patterns $rd myhash hexpired del
            assert_equal -2 [r HTTL myhash FIELDS 1 f1]
            assert_equal 0 [r HLEN myhash]
            assert_equal 0 [r EXISTS myhash]
            assert_equal 0 [get_keys r]
            assert_equal 0 [get_keys_with_volatile_items r]
            assert_equal 1 [info_field [r info stats] expired_fields]
            $rd close
        }
    }

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
        assert_equal 1 [get_keys_with_volatile_items r]
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
    } {ERR *}

    test {HSETEX PX - mismatched field/value count} {
         assert_error {ERR numfields should be greater than 0 and match the provided number of fields} {r HSETEX myhash PX 100 FIELDS 1 field1 val1 extra}
    } 


    ## FNX/FXX

    # hsetex throws ERR *, it shouldn't
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
    } {ERR *}

     test {HSETEX EX - FXX does not create object in case key does not exist} {
        r FLUSHALL
        assert_equal 0 [r HSETEX myhash EX 10 FXX FIELDS 1 x y]
        assert_equal 0 [r EXISTS myhash]
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

    # HEXPIRE on a non-existent field
    test {HEXPIRE on a non-existent field (should not create field)} {
        r FLUSHALL
        r HSET myhash f1 v1
        r HEXPIRE myhash 1000 FIELDS 1 f2
        assert_equal 0 [r HEXISTS myhash f2]
        assert_equal -2 [r HTTL myhash FIELDS 1 f2]
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

    foreach command {HEXPIRE HPEXPIRE HEXPIREAT HPEXPIREAT} {
        test "HSETEX $command 0/past time works correctly with 1 field" {
            r FLUSHALL
            r config resetstat
            # Create hash with field
            r HSET myhash f1 v1
            assert_equal 1 [r HLEN myhash]
            assert_equal 0 [get_keys_with_volatile_items r]
            assert_equal 1 [get_keys r]
            set rd [setup_single_keyspace_notification r]
            
            # Set field to expire immediately
            assert_equal {2} [r $command myhash [get_past_zero_expire_value $command] FIELDS 1 f1]

            # Verify field and keys are deleted
            assert_keyevent_patterns $rd myhash hexpired del
            assert_equal -2 [r HTTL myhash FIELDS 1 f1]
            assert_equal 0 [r HLEN myhash]
            assert_equal 0 [r EXISTS myhash]
            assert_equal 0 [get_keys r]
            assert_equal 0 [get_keys_with_volatile_items r]
            assert_equal 1 [info_field [r info stats] expired_fields]
            $rd close
        }
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
    
    test {HPERSIST, then HEXPIRE, check new TTL is set} {
        r FLUSHALL
        r HSET myhash f1 v1
        r HEXPIRE myhash 1000 FIELDS 1 f1
        assert_equal 1 [r HPERSIST myhash FIELDS 1 f1]
        r HEXPIRE myhash 2000 FIELDS 1 f1
        assert_morethan [r HTTL myhash FIELDS 1 f1] 1000
    }

     #################### HRANDFIELD ##################

    test "HRANDFIELD - CASE 1: negative count" {
        r FLUSHALL
        assert_equal {1} [r HSETEX myhash PX 1 fields 5 f1 v1 f2 v2 f3 v3 f4 v4 f5 v5]
        wait_for_condition 100 100 {
            [r HGETALL myhash] eq {}
        } else {
            fail "Hash is showing expired elements"
        }
        # check that we get an empty response even though there are expired fields
        assert_match {} [r hrandfield myhash 1]

        # Now write a persistent element
        assert_equal {1} [r HSET myhash f5 v5]
        # make sure this is the element we will get all the time
        for {set i 1} {$i <= 50} {incr i} {
            assert_equal {f5 f5 f5 f5 f5} [r hrandfield myhash -5]
        }

    }

     test "HRANDFIELD - CASE 2: The number of requested elements is greater than the number of elements inside the hash" {
        r FLUSHALL
        assert_equal {1} [r HSETEX myhash PX 1 fields 5 f1 v1 f2 v2 f3 v3 f4 v4 f5 v5]
        wait_for_condition 100 100 {
            [r HGETALL myhash] eq {}
        } else {
            fail "Hash is showing expired elements"
        }
        # check that we get an empty response even though there are expired fields
        assert_match {} [r hrandfield myhash 10]

        # Now write a persistent element
        assert_equal {3} [r HSET myhash f5 v5 f6 v6 f7 v7]
        # make sure this is the element we will get all the time
        for {set i 1} {$i <= 50} {incr i} {
            set result [r hrandfield myhash 10]
            assert_equal 3 [llength [split $result]]
            assert_match {*f5*} $result
            assert_match {*f6*} $result
            assert_match {*f7*} $result
        }

    }

     test "HRANDFIELD - CASE 3: The number of elements inside the hash is not greater than 3 times the number of requested elements" {
        r FLUSHALL
        assert_equal {1} [r HSETEX myhash PX 1 fields 5 f1 v1 f2 v2 f3 v3 f4 v4 f5 v5]
        wait_for_condition 100 100 {
            [r HGETALL myhash] eq {}
        } else {
            fail "Hash is showing expired elements"
        }
        # check that we get an empty response even though there are expired fields
        assert_match {} [r hrandfield myhash 4]

        # Now write a persistent elements
        assert_equal {4} [r HSET myhash f5 v5 f6 v6 f7 v7 f8 v8]
        # make sure this is the elements we will get all the time
        for {set i 1} {$i <= 50} {incr i} {
            set result [r hrandfield myhash 4]
            assert_equal 4 [llength [split $result]]
             assert_match {*f5*} $result
             assert_match {*f6*} $result
             assert_match {*f7*} $result
             assert_match {*f8*} $result
        }
    }

    test "HRANDFIELD - CASE 4: The number of elements inside the hash is greater than 3 times the number of requested elements" {
        r FLUSHALL
        assert_equal {1} [r HSETEX myhash PX 1 fields 8 f1 v1 f2 v2 f3 v3 f4 v4 f5 v5 f6 v6 f7 v7 f8 v8]
        wait_for_condition 100 100 {
            [r HGETALL myhash] eq {}
        } else {
            fail "Hash is showing expired elements"
        }
        
        # check that we get an empty response even though there are expired fields
        assert_match {} [r hrandfield myhash 2]

        # Now write a persistent elements
        assert_equal {3} [r HSET myhash f8 v8 f9 v9 f10 v10]
        # make sure this is the elements we will get all the time
        for {set i 1} {$i <= 50} {incr i} {
            set result [r hrandfield myhash 3]
            assert_equal 3 [llength [split $result]]
            assert_match {*f8*} $result
            assert_match {*f9*} $result
            assert_match {*f10*} $result
        }
    }

    foreach cmd {RENAME RESTORE} {
        test "$cmd Preserves Field TTLs" {
            r FLUSHALL
            r DEBUG SET-ACTIVE-EXPIRE 0
            r HSET myhash{t} f1 v1 f2 v2
            r HEXPIRE myhash{t} 200 FIELDS 1 f1

            # Verify initial TTL state
            set mem_before [r MEMORY USAGE myhash{t}]
            assert_equal "v1 v2" [r HMGET myhash{t} f1 f2]
            assert_morethan [r HTTL myhash{t} FIELDS 1 f1] 100
            assert_equal -1 [r HTTL myhash{t} FIELDS 1 f2]
            assert_equal 2 [r HLEN myhash{t}]
            assert_equal 1 [get_keys r]
            assert_equal 1 [get_keys_with_volatile_items r]

            # Run the command
            if {$cmd eq "RENAME"} {
                r rename myhash{t} nwhash{t}
                set newhash nwhash{t}
            } elseif {$cmd eq "RESTORE"} {
                set serialized [r DUMP myhash{t}]
                r RESTORE rstrhs{t} 0 $serialized
                set newhash rstrhs{t}
            }

            # Verify field values and TTLs are preserved
            set memory_after [r MEMORY USAGE $newhash]
            assert_equal "v1 v2" [r HMGET $newhash f1 f2]
            assert_morethan [r HTTL $newhash FIELDS 1 f1] 100
            assert_equal -1 [r HTTL $newhash FIELDS 1 f2]
            assert_equal 2 [r HLEN $newhash]
            if {$cmd eq "RESTORE"} {
                assert_equal 2 [get_keys r]
                assert_equal 2 [get_keys_with_volatile_items r]
            } else {
                assert_equal 1 [get_keys r]
                assert_equal 1 [get_keys_with_volatile_items r]
            }
            assert_equal $mem_before $memory_after
        } {} {needs:debug}
    }

    test {COPY Preserves TTLs} {
        r flushall
        # Create hash with fields
        r HSET myhash{t} f1 v1 f3 v3 f4 v4

        # Set TTL on f1 only
        r HEXPIRE myhash{t} 2000 FIELDS 1 f1
        r HEXPIRE myhash{t} 5000 FIELDS 1 f3

        # Copy hash to new key
        r copy myhash{t} nwhash{t}

        # Verify initial TTL state
        assert_equal [r MEMORY USAGE myhash{t}] [r MEMORY USAGE nwhash{t}]
        assert_equal "v1 v3 v4" [r HMGET myhash{t} f1 f3 f4]
        assert_equal "v1 v3 v4" [r HMGET nwhash{t} f1 f3 f4]
        assert_equal [r HEXPIRETIME myhash{t} FIELDS 1 f1] [r HEXPIRETIME nwhash{t} FIELDS 1 f1] 
        assert_equal [r HEXPIRETIME myhash{t} FIELDS 1 f2] [r HEXPIRETIME nwhash{t} FIELDS 1 f2]
        assert_equal [r HEXPIRETIME myhash{t} FIELDS 1 f3] [r HEXPIRETIME nwhash{t} FIELDS 1 f3]
        assert_equal [r HEXPIRETIME myhash{t} FIELDS 1 f4] [r HEXPIRETIME nwhash{t} FIELDS 1 f4]
    }

    test {Hash Encoding Transitions with TTL - Add TTL to Existing Fields} {
        r flushall
        r DEBUG SET-ACTIVE-EXPIRE 0
        
        # Create small hash with listpack encoding
        r HSET myhash f1 v1 f2 v2
        
        # Verify initial encoding
        set "listpack" [r OBJECT ENCODING myhash]
        
        # Add TTL to existing field
        r HEXPIRE myhash 300 FIELDS 1 f1
        
        # Verify encoding changed to hashtable
        set "hashtable" [r OBJECT ENCODING myhash]

        # Verify field values are preserved
        assert_equal "v1 v2" [r HMGET myhash f1 f2]
        # Veridy expiry
        assert_morethan [r HTTL myhash FIELDS 1 f1] 100
        assert_equal -1 [r HTTL myhash FIELDS 1 f2]
        # Re-enable active expiry
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}
    
    test {Hash Encoding Transitions with TTL - Create New Fields with TTL} {
        r flushall
        r DEBUG SET-ACTIVE-EXPIRE 0
        
        # Create small hash with listpack encoding
        r HSET myhash f1 v1 f2 v2
        
        # Verify initial encoding
        set "listpack" [r OBJECT ENCODING myhash]
        
        # Add many fields to force encoding transition
        for {set i 3} {$i <= 600} {incr i} {
            lappend pairs "f$i" "v$i"
        }
        r HSET myhash {*}$pairs
        
        set expire_time [get_long_expire_value HEXPIREAT]
        r HEXPIREAT myhash $expire_time FIELDS 5 f1 f10 f100 f200 f300
        
        # Verify encoding changed to hashtable
        set "hashtable" [r OBJECT ENCODING myhash]
        
        # Verify all field values and TTLs are correct
        for {set i 1} {$i <= 600} {incr i} {
            assert_equal "v$i" [r HGET myhash "f$i"]
            if {$i == 1 || $i == 10 || $i == 100 || $i == 200 || $i == 300} {
                assert_equal [r HEXPIRETIME myhash FIELDS 1 "f$i"] $expire_time
            } else {
                assert_equal [r HTTL myhash FIELDS 1 "f$i"] -1
            }
        }
        # Re-enable active expiry
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {HGETALL skips expired fields} {
        r FLUSHALL
        r DEBUG SET-ACTIVE-EXPIRE 0

        # Set two fields: one persistent, one with short TTL
        r HSET myhash persistent "val1"
        r HSETEX myhash PX 5 FIELDS 1 expiring "val2"

        # Wait for expiry to pass
        after 10      

        # HGETALL should skip expired field
        set result [r HGETALL myhash]
        assert_equal {persistent val1} $result

        # Re-enable active expiry
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {HSCAN skips expired fields} {
        r FLUSHALL
        r DEBUG SET-ACTIVE-EXPIRE 0

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
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

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
    } {} {needs:debug}

    test {HSET - overwrite expired field without TTL clears expiration} {
        r FLUSHALL
        r DEBUG SET-ACTIVE-EXPIRE 0

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

        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {HINCRBY - on expired field} {
        r FLUSHALL
        r DEBUG SET-ACTIVE-EXPIRE 0

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
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {HINCRBYFLOAT - on expired field} {
        r FLUSHALL
        r DEBUG SET-ACTIVE-EXPIRE 0

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
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {HSET - overwrite unexpired field removes TTL} {
        r FLUSHALL
        r DEBUG SET-ACTIVE-EXPIRE 0

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

        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {HDEL - expired field is removed without triggering expiry logic} {
        r FLUSHALL
        r DEBUG SET-ACTIVE-EXPIRE 0

        # This test proves that deleting an expired field with HDEL
        # does NOT trigger Valkey's expiration mechanism.
        #
        # The key observation is that Valkey tracks how many fields were
        # expired via TTL using the `expired_fields` counter in INFO stats.
        # If HDEL caused expiration to be processed internally,
        # this counter would increment. We assert that it remains unchanged.

        # Capture expired_fields before
        set before_info [r INFO stats]
        set before [info_field $before_info expired_fields]

        # Create field with short TTL
        r HSETEX myhash PX 10 FIELDS 1 field1 val1
        after 20

        # Field is technically expired, but still in-memory due to lazy expiry
        assert_equal 1 [r HLEN myhash]

        # Delete the expired field directly
        r HDEL myhash field1

        # Field should be gone
        assert_equal 0 [r HEXISTS myhash field1]

        # Capture expired_fields again
        set after_info [r INFO stats]
        set after [info_field $after_info expired_fields]

        # Verify that no expiry occurred internally
        assert_equal $before $after
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {HDEL on field with TTL, then re-add and check TTL is gone} {
        r FLUSHALL
        r HSET myhash f1 v1
        r HEXPIRE myhash 10000 FIELDS 1 f1
        assert_morethan [r HTTL myhash FIELDS 1 f1] 0
        r HDEL myhash f1
        r HSET myhash f1 v2
        assert_equal -1 [r HTTL myhash FIELDS 1 f1]
    }

    ## expired_fields Tests ####
    test {expired_fields metric increments by one when single hash field expires} {
        r FLUSHALL
        set initial_expired [info_field [r info stats] expired_fields]
        
        # Create hash with fields and ttl
        r HSET myhash f1 v1 f2 v2 f3 v3
        assert_equal 3 [r HLEN myhash]

        # Force expiration by setting very short TTL
        r HPEXPIRE myhash 1 FIELDS 1 f1
        
        # Wait for expiration
        wait_for_active_expiry r myhash 2 $initial_expired 1
         
        # Verify expired field returns empty string and non-expired return values
        assert_equal "{} v2 v3" [r HMGET myhash f1 f2 f3]
    }

    test {expired_fields metric tracks multiple field expirations with keyspace notifications} {
        r FLUSHALL
        set initial_expired [info_field [r info stats] expired_fields]
        
        set rd [setup_single_keyspace_notification r]
        
        # Create hash with expiring fields
        r HSET myhash f1 v1 f2 v2 f3 v3 f4 v4 f5 v5
        r HEXPIRE myhash 1000 FIELDS 1 f1
        r HEXPIRE myhash 2000 FIELDS 1 f2
        
        # Force expiration with short ttl
        r HPEXPIRE myhash 1 FIELDS 1 f1
        
        # Wait for expiration
        wait_for_active_expiry r myhash 4 $initial_expired 1
        
        # Verify expired_fields incremented
        assert_equal 1 [expr {[info_field [r info stats] expired_fields] - $initial_expired}]
        
        # Verify expired field returns empty string and non-expired return values
        assert_equal "{} v2 v3 v4 v5" [r HMGET myhash f1 f2 f3 f4 f5]
        
        # Test HPERSIST remove TTL from f2
        r HPERSIST myhash FIELDS 1 f2
        
        # Verify f2 no longer has TTL
        assert_equal -1 [r HTTL myhash FIELDS 1 f2]
        assert_equal 1 [expr {[info_field [r info stats] expired_fields] - $initial_expired}]

        # Expire 2 fields at once
        r HPEXPIRE myhash 1 FIELDS 2 f4 f5
        wait_for_active_expiry r myhash 2 $initial_expired 3
        assert_equal 3 [expr {[info_field [r info stats] expired_fields] - $initial_expired}]
        
        # Verify expired fields return empty string and non-expired return values
        assert_equal "{} v2 v3 {} {}" [r HMGET myhash f1 f2 f3 f4 f5]

        # Wait for hset and hexpire events
        assert_keyevent_patterns $rd myhash hset hexpire hexpire hexpire hexpired hpersist hexpire hexpired
        $rd close
    }

    foreach time_unit {s, ms} {
        test "Key TTL expires before field TTL: entire hash should be deleted timeunit: $time_unit" {
            r FLUSHALL
            r DEBUG SET-ACTIVE-EXPIRE 0
            r config set notify-keyspace-events KEA
            set rd [valkey_deferring_client]
            assert_equal {1} [psubscribe $rd __keyevent@*]

            r HSET myhash f1 v1 f2 v2 f3 v3
            assert_equal 1 [get_keys r]
            assert_equal 0 [get_keys_with_volatile_items r]
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
            assert_equal 0 [get_keys r]

            assert_keyevent_patterns $rd myhash hset hexpire expire
            assert_equal 0 [get_keys_with_volatile_items r]
            $rd close
            # Re-enable active expiry
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "Field TTL expires before key TTL: only the specific field should expire: $time_unit" {
            r FLUSHALL
            r DEBUG SET-ACTIVE-EXPIRE 0
            set rd [valkey_deferring_client]
            assert_equal {1} [psubscribe $rd __keyevent@*]
            
            r HSET myhash f1 v1 f2 v2 f3 v3
            assert_equal 1 [get_keys r]
            assert_equal 0 [get_keys_with_volatile_items r]
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
            assert_equal 1 [get_keys r]
            assert_equal 1 [r EXISTS myhash]
            assert_equal "{} v2 v3" [r HMGET myhash f1 f2 f3]
            assert_keyevent_patterns $rd myhash hset hexpire
            # When active expire is disabled, expired key is 
            # not deleted and get_keys_with_volatile_items is the same
            assert_equal 1 [get_keys_with_volatile_items r]
            $rd close
            # Re-enable active expiry
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test "Key and field TTL expire simultaneously: entire hash should be deleted: $time_unit" {
            r FLUSHALL
            r DEBUG SET-ACTIVE-EXPIRE 0
            
            r HSET myhash f1 v1 f2 v2 f3 v3
            assert_equal 1 [get_keys r]
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
                fail "myhash still exist"
            }

            assert_equal "{} {} {}" [r HMGET myhash f1 f2 f3]
            assert_equal 0 [get_keys r]
            assert_equal 0 [r HLEN myhash]
            # Re-enable active expiry
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test {Millisecond/Seconds precision} {
            r flushall
            r DEBUG SET-ACTIVE-EXPIRE 0

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
            # Re-enable active expiry
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}
    }

    test {Ensure that key-level PERSIST on the key don't affect field TTL} {
        r FLUSHALL
        
        r HSET myhash f1 v1 f2 v2
        assert_equal 1 [get_keys r]
        assert_equal 0 [get_keys_with_volatile_items r]
        assert_equal 2 [r HLEN myhash]
        r HEXPIRE myhash 100000 FIELDS 1 f1
        r PERSIST myhash
        
        assert_equal -1 [r TTL myhash]
        assert_morethan [r HTTL myhash FIELDS 1 f1] 0
        assert_equal 1 [get_keys_with_volatile_items r]
    }

    test {Verify error when hash expire commands num fields is not provided} {
        r FLUSHALL
        catch {r hsetex myhash KEEPTTL KEEPTTL KEEPTTL FIELDS} e
        assert_match $e {ERR numfields should be greater than 0 and match the provided number of fields}
        catch {r hexpire myhash 10 NX NX FIELDS} e
        assert_match $e {ERR numfields should be greater than 0 and match the provided number of fields}
        catch {r hgetex myhash PERSIST PERSIST FIELDS} e
        assert_match $e {ERR numfields should be greater than 0 and match the provided number of fields}
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

start_server {tags {"hashexpire external:skip"}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    start_server {tags {needs:repl external:skip}} {
        set replica_1 [srv 0 client]
        set replica_1_host [srv 0 host]
        set replica_1_port [srv 0 port]

        test {Replication Primary -> R1} {
            lassign [setup_replication_test $primary $replica_1 $primary_host $primary_port] primary_initial_expired replica_1_initial_expired

            # Initialize deferred clients and subscribe to keyspace notifications
            foreach instance [list $primary $replica_1] {
                $instance config set notify-keyspace-events KEA
            }
            set rd_primary [valkey_deferring_client -1]
            set rd_replica_1 [valkey_deferring_client $replica_1_host $replica_1_port]
            foreach rd [list $rd_primary $rd_replica_1] {
                assert_equal {1} [psubscribe $rd __keyevent@*]
            }


            # Setup hash, set expire and set expire 0
            $primary HSET myhash f1 v1 f2 v2 ;# Should trigger 3 hset
            # Create hash and timing - f1 < f2 expiry times
            set f1_exp [expr {[clock seconds] + 10000}]
            $primary HEXPIREAT myhash $f1_exp FIELDS 1 f1 ;# Should trigger 3 hexpire
            wait_for_ofs_sync $primary $replica_1
            
            $primary HEXPIRE myhash 0 FIELDS 1 f1 ;# Should trigger 1 hexpired (for primary) and 1 hdel (for replica)
            wait_for_ofs_sync $primary $replica_1

            # Wait for f1 expiration
            wait_for_condition 50 100 {
                [$primary HTTL myhash FIELDS 1 f1] eq -2 && \
                [$replica_1 HTTL myhash FIELDS 1 f1] eq -2
            } else {
                fail "f1 still exists"
            }
            
            # Verify keyspace notification
            foreach rd  [list $rd_primary $rd_replica_1] {
                assert_keyevent_patterns $rd myhash hset hexpire
            }
            # primary gets hexpired and replica gets hdel
            assert_keyevent_patterns $rd_primary myhash hexpired
            assert_keyevent_patterns $rd_replica_1 myhash hdel

            $rd_primary close
            $rd_replica_1 close
        }

        start_server {tags {needs:repl external:skip}} {
            $primary FLUSHALL
            set replica_2 [srv 0 client]
            set replica_2_host [srv 0 host]
            set replica_2_port [srv 0 port]
            
            test {Chain Replication (Primary -> R1 -> R2) preserves TTL} {
                $replica_1 replicaof $primary_host $primary_port
                # Wait for R2 to connect to R1
                wait_for_condition 100 100 {
                    [info_field [$replica_1 info replication] master_link_status] eq "up"
                } else {
                    fail "R1 <-> PRIMARY didn't establish connection"
                }

                $replica_2 replicaof $replica_1_host $replica_1_port
                # Wait for R2 to connect to R1
                wait_for_condition 100 100 {
                    [info_field [$replica_1 info replication] master_link_status] eq "up"
                } else {
                    fail "R2 <-> R1 didn't establish connection"
                }

                # Initialize deferred clients and subscribe to keyspace notifications
                set rd_primary [valkey_deferring_client -2]
                set rd_replica_1 [valkey_deferring_client -1]
                set rd_replica_2 [valkey_deferring_client $replica_2_host $replica_2_port]
                assert_equal {1} [psubscribe $rd_primary __keyevent@*]
                assert_equal {1} [psubscribe $rd_replica_1 __keyevent@*]
                assert_equal {1} [psubscribe $rd_replica_2 __keyevent@*]
    
                # Create hash and timing - f1 < f2 < f3 expiry times
                set f1_exp [expr {[clock seconds] + 1000000}]

                wait_for_ofs_sync $primary $replica_1
                wait_for_ofs_sync $replica_1 $replica_2

                ############################################# STEUP HASH #############################################
                $primary HSETEX myhash FIELDS 2 f1 v1 f2 v2 ;# Should trigger 3 hset
                wait_for_ofs_sync $primary $replica_1
                wait_for_ofs_sync $replica_1 $replica_2

                # Verify hset event was generated on all 3 nodes
                foreach rd [list $rd_primary $rd_replica_1 $rd_replica_2] {
                    assert_keyevent_patterns $rd myhash hset
                }

                $primary HEXPIREAT myhash $f1_exp FIELDS 1 f1 ;# Should trigger 3 hexpire
                wait_for_ofs_sync $primary $replica_1
                wait_for_ofs_sync $replica_1 $replica_2

                # Verify hexpire event was generated on all 3 nodes
                foreach rd [list $rd_primary $rd_replica_1 $rd_replica_2] {
                    assert_keyevent_patterns $rd myhash hexpire
                }
                
                $primary HPEXPIRE myhash 0 FIELDS 1 f1 ;# Should trigger 1 hexpired (for primary) and 2 hdel (for replicas)
                wait_for_ofs_sync $primary $replica_1
                wait_for_ofs_sync $replica_1 $replica_2


                # Wait for f1 expiration
                wait_for_condition 50 100 {
                    [$primary HTTL myhash FIELDS 1 f1] eq -2 && \
                    [$replica_1 HTTL myhash FIELDS 1 f1] eq -2 && \
                    [$replica_2 HTTL myhash FIELDS 1 f1] eq -2
                } else {
                    fail "f1 still exists"
                }
                
                # primary gets hexpired and replicas get hdel
                assert_keyevent_patterns $rd_primary myhash hexpired
                assert_keyevent_patterns $rd_replica_1 myhash hdel
                assert_keyevent_patterns $rd_replica_2 myhash hdel

                $rd_primary close
                $rd_replica_1 close
                $rd_replica_2 close
            }
        }

        test {Replica Failover} {
            $primary FLUSHALL
            $primary DEBUG SET-ACTIVE-EXPIRE 0
            $replica_1 DEBUG SET-ACTIVE-EXPIRE 0
            ####### Replication setup #######
            $replica_1 replicaof $primary_host $primary_port
            wait_for_condition 50 100 {
                [lindex [$replica_1 role] 0] eq {slave} &&
                [string match {*master_link_status:up*} [$replica_1 info replication]]
            } else {
                fail "Can't turn the instance into a replica"
            }
            
            # Create hash fields with TTL on primary
            set f1_exp [expr {[clock seconds] + 200}]
            set f2_exp [expr {[clock seconds] + 300000}]
            $primary HSET myhash f1 v1 f2 v2 f3 v3
            $primary HEXPIREAT myhash $f1_exp FIELDS 1 f1
            $primary HEXPIREAT myhash $f2_exp FIELDS 1 f2
            # f3 remains persistent

            # Wait for full sync
            wait_for_ofs_sync $primary $replica_1

            # Verify primary and replica are the same
            foreach instance [list $primary $replica_1] {
                assert_equal $f1_exp [$instance HEXPIRETIME myhash FIELDS 1 f1]
                assert_equal $f2_exp [$instance HEXPIRETIME myhash FIELDS 1 f2]
                assert_equal -1 [$instance HTTL myhash FIELDS 1 f3]
                assert_equal 1 [get_keys $instance]
                assert_equal 1 [get_keys_with_volatile_items $instance]
                assert_equal "v1 v2 v3" [$instance HMGET myhash f1 f2 f3]
                assert_equal 3 [$instance HLEN myhash]
            }

            # Perform failover
            $replica_1 replicaof no one
            # Wait for replica to become primary
            wait_for_condition 100 100 {
                [info_field [$replica_1 info replication] role] eq "master"
            } else {
                fail "Replica didn't become master"
            }

            # Setup keyspace notifications for the promoted replica
            $replica_1 config set notify-keyspace-events KEA
            set rd_replica [valkey_deferring_client $replica_1_host $replica_1_port]
            assert_equal {1} [psubscribe $rd_replica __keyevent@*]

            # Check all values that checked before are the same
            assert_equal 3 [$replica_1 HLEN myhash]
            assert_equal $f1_exp [$replica_1 HEXPIRETIME myhash FIELDS 1 f1]
            assert_equal $f2_exp [$replica_1 HEXPIRETIME myhash FIELDS 1 f2]
            assert_equal -1 [$replica_1 HTTL myhash FIELDS 1 f3]
            assert_equal "v1 v2 v3" [$replica_1 HGETEX myhash FIELDS 3 f1 f2 f3]
            assert_equal 3 [$replica_1 HLEN myhash]
            
            # Set f1 to expire in 1 second and wait for expiration
            $replica_1 HEXPIRE myhash 1 FIELDS 1 f1 ;# will trigger hexpire
            wait_for_condition 50 100 {
                [$replica_1 HTTL myhash FIELDS 1 f1] eq -2
            } else {
                fail "f1 not expired"
            }

            # Verify expiry in replica
            assert_equal "" [$replica_1 HGET myhash f1]
            assert_equal 3 [$replica_1 HLEN myhash]

            # Verify no expiry in primary
            assert_equal "v1" [$primary HGET myhash f1]

            # Change TTL of f2
            $replica_1 HEXPIRE myhash 1000000 FIELDS 1 f2 ;# will trigger hexpire
            assert_morethan [$replica_1 HTTL myhash FIELDS 1 f2] 9000
            assert_equal $f2_exp [$primary HEXPIRETIME myhash FIELDS 1 f2]
            
            # Change TTL of f2 to 0 (immediate expiry)
            $replica_1 HGETEX myhash EX 0 FIELDS 1 f2 ;# will trigger hexpired
            # Verify final state
            assert_equal 2 [$replica_1 HLEN myhash]
            assert_equal "{} {} v3" [$replica_1 HGETEX myhash FIELDS 3 f1 f2 f3]
            assert_equal "v1 v2 v3" [$primary HGETEX myhash FIELDS 3 f1 f2 f3] ;# No change for primary

            assert_keyevent_patterns $rd_replica myhash hexpire hexpire hexpired

            $rd_replica close
            # Re-enable active expiry
            $primary DEBUG SET-ACTIVE-EXPIRE 1
            $replica_1 DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}
        

        test {Promotion to primary} {
            lassign [setup_replication_test $primary $replica_1 $primary_host $primary_port] primary_initial_expired replica_1_initial_expired

            # Initialize deferred clients and subscribe to keyspace notifications
            foreach instance [list $primary $replica_1] {
                $instance config set notify-keyspace-events KEA
                $instance DEBUG SET-ACTIVE-EXPIRE 0
            }
            ####### Replication setup #######
            $replica_1 replicaof $primary_host $primary_port
            wait_for_condition 50 100 {
                [lindex [$replica_1 role] 0] eq {slave} &&
                [string match {*master_link_status:up*} [$replica_1 info replication]]
            } else {
                fail "Can't turn the instance into a replica"
            }
            
            # Create hash fields with TTL on primary
            set f1_exp [expr {[clock seconds] + 200}]
            set f2_exp [expr {[clock seconds] + 300000}]
            $primary HSET myhash f1 v1 f2 v2 f3 v3
            $primary HEXPIREAT myhash $f1_exp FIELDS 1 f1
            $primary HEXPIREAT myhash $f2_exp FIELDS 1 f2
            # f3 remains persistent

            # Wait for full sync
            wait_for_ofs_sync $primary $replica_1

            # Verify primary and replica are the same
            foreach instance [list $primary $replica_1] {
                assert_equal $f1_exp [$instance HEXPIRETIME myhash FIELDS 1 f1]
                assert_equal $f2_exp [$instance HEXPIRETIME myhash FIELDS 1 f2]
                assert_equal -1 [$instance HTTL myhash FIELDS 1 f3]
                assert_equal 1 [get_keys $instance]
                assert_equal 1 [get_keys_with_volatile_items $instance]
                assert_equal "v1 v2 v3" [$instance HMGET myhash f1 f2 f3]
                assert_equal 3 [$instance HLEN myhash]
            }

            # Perform promotion to primary
            $primary FAILOVER TO $replica_1_host $replica_1_port
            # Wait for replica to become primary
            wait_for_condition 100 100 {
                [info_field [$replica_1 info replication] role] eq "master"
            } else {
                fail "Replica didn't become master"
            }

            # Setup keyspace notifications
            $primary config set notify-keyspace-events KEA
            $replica_1 config set notify-keyspace-events KEA
            set rd_primary [valkey_deferring_client -1]
            set rd_replica_1 [valkey_deferring_client $replica_1_host $replica_1_port]
            assert_equal {1} [psubscribe $rd_primary __keyevent@*]
            assert_equal {1} [psubscribe $rd_replica_1 __keyevent@*]

            # Check all values that checked before are the same after the failover
            foreach instance [list $primary $replica_1] {
                assert_equal $f1_exp [$instance HEXPIRETIME myhash FIELDS 1 f1]
                assert_equal $f2_exp [$instance HEXPIRETIME myhash FIELDS 1 f2]
                assert_equal -1 [$instance HTTL myhash FIELDS 1 f3]
                assert_equal 1 [get_keys $instance]
                assert_equal 1 [get_keys_with_volatile_items $instance]
                assert_equal "v1 v2 v3" [$instance HMGET myhash f1 f2 f3]
                assert_equal 3 [$instance HLEN myhash]
            }
            
            # Set f1 to expire in 1 second and wait for expiration
            $replica_1 HEXPIRE myhash 1 FIELDS 1 f1 ;# will trigger hexpire
            wait_for_ofs_sync $replica_1 $primary
            wait_for_condition 50 100 {
                [$replica_1 HTTL myhash FIELDS 1 f1] eq -2
            } else {
                fail "f1 not expired"
            }

            # Verify replica and primary are sync
            foreach instance [list $primary $replica_1] {
                assert_equal $f2_exp [$instance HEXPIRETIME myhash FIELDS 1 f2]
                assert_equal -2 [$instance HTTL myhash FIELDS 1 f1]
                assert_equal 1 [get_keys $instance]
                assert_equal 1 [get_keys_with_volatile_items $instance]
                assert_equal "{} v2 v3" [$instance HMGET myhash f1 f2 f3]
                assert_equal 3 [$instance HLEN myhash]
            }

            # Change TTL of f2
            $replica_1 HEXPIRE myhash 1000000 FIELDS 1 f2 ;# will trigger hexpire
            wait_for_ofs_sync $replica_1 $primary
            foreach instance [list $primary $replica_1] {
                assert_morethan [$instance HTTL myhash FIELDS 1 f2] 9000
            }
            
            # Change TTL of f2 to 0 (immediate expiry)
            $replica_1 HGETEX myhash EX 0 FIELDS 1 f2 ;# will trigger hexpired for replica_1 and hdel for primary
            # Verify final state
            wait_for_ofs_sync $replica_1 $primary
            foreach instance [list $primary $replica_1] {
                assert_equal 2 [$instance HLEN myhash]
                assert_equal "{} {} v3" [r HMGET myhash f1 f2 f3]
            }

            foreach rd [list $rd_replica_1 $rd_primary] {
                assert_keyevent_patterns $rd myhash hexpire hexpire
            }
            assert_keyevent_patterns $rd_replica_1 myhash hexpired
            assert_keyevent_patterns $rd_primary myhash hdel

            $rd_replica_1 close
            $rd_primary close
            # Re-enable active expiry
            $primary DEBUG SET-ACTIVE-EXPIRE 1
            $replica_1 DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}
    }
}

### Slot Migration ####
start_cluster 3 0 {tags {"cluster mytest external:skip"} overrides {cluster-node-timeout 1000}} {
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
        R 0 DEBUG SET-ACTIVE-EXPIRE 0
        R 1 DEBUG SET-ACTIVE-EXPIRE 0
        # Create hash fields
        R 0 HSET $key f1 v1 f2 v2 f3 v3

        # Set TTL on fields f1 and f2
        R 0 HEXPIRE $key 300 FIELDS 2 f1 f2

        # Verify before slot migration
        assert_equal 3 [R 0 HLEN $key]
        assert_morethan [R 0 HTTL $key FIELDS 1 f1] 290
        assert_match {1} [scan [regexp -inline {keys\=([\d]*)} [R 0 info keyspace]] keys=%d]
        assert_equal 1 [scan [lindex [regexp -inline {keys_with_volatile_items=([\d]+)} [R 0 info keyspace]] 1] "%d"]
        
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
        assert_equal 1 [scan [lindex [regexp -inline {keys_with_volatile_items=([\d]+)} [R 1 info keyspace]] 1] "%d"]

        # Setup keyspace notifications
        R 1 config set notify-keyspace-events KEA
        set rd [valkey_deferring_client -1]
        assert_equal {1} [psubscribe $rd __keyevent@0__:hexpired]

        # Set expiration to 0
        R 1 HGETEX $key EX 0 FIELDS 1 f1
        
        # Veridy expiration
        assert_keyevent_patterns $rd "{$key}" hexpired
        assert_equal 2 [R 1 HLEN $key]
        assert_equal "" [R 1 HGET $key f1]
        assert_equal -2 [R 1 HTTL $key FIELDS 1 f1]

        $rd close
        # Re-enable active expiry
        R 0 DEBUG SET-ACTIVE-EXPIRE 1
        R 1 DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}
}

#### AOF Test #####
proc validate_aof_content {aof_file pxat_count hdel_count} {
    wait_for_condition 100 100 {
        [file exists $aof_file] eq 1
    } else {
        fail "hash value was not expired after timeout"
    }

    set aof_content [exec cat $aof_file]

    # Verify amount of PXAT and HDEL
    # Count PXAT commands
    set got_pxat_count [regexp -all {PXAT} $aof_content]
    assert_equal $got_pxat_count $pxat_count
    # Count HDEL commands
    set got_hdel_count [regexp -all {HDEL} $aof_content]
    assert_equal $got_hdel_count $hdel_count
}
tags {"aof external:skip"} {
    foreach rdb_preamble {"yes" "no"} {
        set defaults {appendonly {yes} appendfilename {appendonly.aof} appenddirname {appendonlydir} auto-aof-rewrite-percentage {0}}
        set server_path [tmpdir server.multi.aof]
        start_server_aof [list dir $server_path aof-use-rdb-preamble $rdb_preamble] {
            set rdb_preamble [lindex [r config get aof-use-rdb-preamble] 1]
            test "TTL Persistence in AOF aof-use-rdb-preamble $rdb_preamble" {
                r flushall
                r DEBUG SET-ACTIVE-EXPIRE 0
                r config set appendonly yes
                r config set appendfsync always
                assert_equal 0 [get_keys_with_volatile_items r]

                # Create hash with 1 short, long and no expired fields
                set long_expire [expr {[clock seconds] + 1000000}]
                # Create 10 fields with long expiry
                for {set i 1} {$i <= 10} {incr i} {
                    r HSETEX myhash EXAT $long_expire FIELDS 1 f$i v$i ;# 10 PXAT to aof
                }

                # Create 10 fields with short expiry
                for {set i 11} {$i <= 20} {incr i} {
                    r HSETEX myhash PXAT [expr {[clock milliseconds] + 10}] FIELDS 1 f$i v$i ;# 10 PXAT to aof
                }

                # Create 10 fields with expire 0
                for {set i 21} {$i <= 30} {incr i} {
                    r HSET myhash f$i v$i
                    r HEXPIRE myhash 0 FIELDS 1 f$i ;# 10 HDEL to aof
                }

                # Create 10 fields with no expiry
                for {set i 31} {$i <= 40} {incr i} {
                    r HSET myhash f$i v$i
                }

                # Now wait for expire of the short expiry
                for {set i 11} {$i <= 20} {incr i} {
                    wait_for_condition 100 100 {
                        [r HTTL myhash FIELDS 1 f$i] eq "-2"
                    } else {
                        fail "hash value was not expired after timeout"
                    }
                }

                # Verify initial HLEN
                assert_equal 30 [r HLEN myhash]
                # Verify values
                for {set i 1} {$i <= 40} {incr i} {
                    if {$i >= 11 && $i <= 30} {
                        assert_equal "" [r HGET myhash f$i]
                    } else {
                        assert_equal v$i [r HGET myhash f$i]
                    }
                }
                assert_equal 1 [get_keys_with_volatile_items r]

                # Ensure the initial rewrite finishes
                waitForBgrewriteaof r

                # Get the last incremental AOF file path and validate its content
                # Count PXAT commands (should be 20: 10 long + 10 short)
                # Count HDEL commands (should be 10: from expire 0)
                validate_aof_content [get_last_incr_aof_path r] 20 10

                # Restart the server and load the AOF
                restart_server 0 true false
                r DEBUG SET-ACTIVE-EXPIRE 0
                r debug loadaof

                set hlen [r HLEN myhash]
                set expired_fields [info_field [r info stats] expired_fields]
                assert_equal 1 [get_keys_with_volatile_items r]

                # Verify that HLEN is between 20 and 30 (inclusive), and 
                # when combined with expired_fields, the total should be 30
                if {$hlen < 20 || $hlen > 30} {
                    fail "Expected HLEN to be between 20 and 30, but got $hlen"
                }
                assert_equal 30 [expr ($expired_fields + $hlen)]

                # Verify the TTLs are preserved
                for {set i 1} {$i <= 10} {incr i} {
                    assert_equal $long_expire [r HEXPIRETIME myhash FIELDS 1 f$i]
                    assert_equal v$i [r HGET myhash f$i]
                }
                # Verify expired fields
                for {set i 11} {$i <= 30} {incr i} {
                    assert_equal -2 [r HTTL myhash FIELDS 1 f$i]
                    assert_equal "" [r HGET myhash f$i]
                }
                # Verify fields with no TTL
                for {set i 31} {$i <= 40} {incr i} {
                    assert_equal -1 [r HTTL myhash FIELDS 1 f$i]
                    assert_equal v$i [r HGET myhash f$i]
                }

                # Trigger and wait for a second rewrite
                r BGREWRITEAOF
                waitForBgrewriteaof r

                if {"$rdb_preamble" eq "no"} {
                    # Get the last base AOF file path and validate its content
                    # Count PXAT commands (should be 10: just the long expiry fields)
                    # Count HDEL commands (should be rewritten out)
                    validate_aof_content [get_base_aof_path r] 10 0
                }

                # Restart the server and load the AOF
                restart_server 0 true false
                r DEBUG SET-ACTIVE-EXPIRE 0
                r debug loadaof

                set hlen [r HLEN myhash]
                set expired_fields [info_field [r info stats] expired_fields]
                assert_equal 1 [get_keys_with_volatile_items r]

                # Verify that HLEN is 20, and we should now have no expired fields
                if {$hlen != 20} {
                    fail "Expected HLEN to be 20, but got $hlen"
                }
                assert_equal 20 [expr ($expired_fields + $hlen)]

                # Verify the TTLs are preserved
                for {set i 1} {$i <= 10} {incr i} {
                    assert_equal $long_expire [r HEXPIRETIME myhash FIELDS 1 f$i]
                    assert_equal v$i [r HGET myhash f$i]
                }
                # Verify expired fields
                for {set i 11} {$i <= 30} {incr i} {
                    assert_equal -2 [r HTTL myhash FIELDS 1 f$i]
                    assert_equal "" [r HGET myhash f$i]
                }
                # Verify fields with no TTL
                for {set i 31} {$i <= 40} {incr i} {
                    assert_equal -1 [r HTTL myhash FIELDS 1 f$i]
                    assert_equal v$i [r HGET myhash f$i]
                }

                # Re-enable active expiry
                r DEBUG SET-ACTIVE-EXPIRE 1
            } {OK} {needs:debug}
        }
    }
}

### ACTIVE EXPIRY TESTS ####
##### HGETEX Active Expiry Tests #####
start_server {tags {"hashexpire external:skip"}} {
    r config set notify-keyspace-events KEA

    foreach command {EX PX EXAT PXAT} {
        test "HGETEX $command active expiry with single field" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]

            r HSET myhash f1 v1 f2 v2
            assert_equal 2 [r HLEN myhash]
            assert_equal 0 [get_keys_with_volatile_items r]
            
            # Use HGETEX to set expiry
            assert_equal "v1" [r HGETEX myhash $command [get_short_expire_value $command] FIELDS 1 f1]
            wait_for_active_expiry r myhash 1 $initial_expired 1
            assert_equal "{} v2" [r HGETEX myhash FIELDS 2 f1 f2]
            assert_equal 0 [get_keys_with_volatile_items r]
        }

        test "HGETEX $command active expiry with multiple fields" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]
            
            r HSET myhash f1 v1 f2 v2 f3 v3
            assert_equal 3 [r HLEN myhash]
            assert_equal 0 [get_keys_with_volatile_items r]
            
            # Set expiry on multiple fields with HGETEX
            assert_equal "v1 v3" [r HGETEX myhash $command [get_short_expire_value $command] FIELDS 2 f1 f3]
                        
            wait_for_active_expiry r myhash 1 $initial_expired 2
            
            # Verify only non-expired field remains
            assert_equal "{} v2 {}" [r HGETEX myhash FIELDS 3 f1 f2 f3]
            assert_equal 0 [get_keys_with_volatile_items r]
        }

        test "HGETEX $command active expiry removes entire key when last field expires" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]
            r HSET myhash f1 v1
            assert_equal 0 [get_keys_with_volatile_items r]
            assert_equal "v1" [r HGETEX myhash $command [get_short_expire_value $command] FIELDS 1 f1]
            wait_for_active_expiry r myhash 0 $initial_expired 1
            assert_equal 0 [r EXISTS myhash]
            assert_equal 0 [get_keys_with_volatile_items r]
        }

        test "HGETEX $command and HPEXPIRE" {
            r FLUSHALL
            r HSET myhash f1 v1 f2 v2 f3 v3 f4 v4
            r HEXPIRE myhash 3000 FIELDS 1 f1
            r HSETEX myhash EX 5000 FIELDS 1 f2 v2
            r HEXPIRE myhash 60000 FIELDS 1 f3
            assert_equal "v1 v2 v3 v4" [r HGETEX myhash FIELDS 4 f1 f2 f3 f4]
            assert_equal "v3" [r HGETEX myhash PERSIST FIELDS 1 f3]
            r HPEXPIRE myhash 1 FIELDS 1 f1
        }
    }

    test "HGETEX PERSIST removes expiry and prevents active expiry" {
        r FLUSHALL

        r HSET myhash f1 v1 f2 v2
        assert_equal 2 [r HLEN myhash]
        assert_equal 0 [get_keys_with_volatile_items r]
        
        # Set short expiry
        assert_equal "v1" [r HGETEX myhash PX 1000 FIELDS 1 f1]
        
        # Immediately persist to prevent expiry
        assert_equal "v1" [r HGETEX myhash PERSIST FIELDS 1 f1]
        assert_equal -1 [r HTTL myhash FIELDS 1 f1]
        
        # Wait longer than original expiry time
        after 200
        
        # Field should still exist due to PERSIST
        assert_equal "v1" [r HGET myhash f1]
        assert_equal 2 [r HLEN myhash]
        assert_equal 0 [get_keys_with_volatile_items r]
    }

    test "HGETEX overwrite existing expiry with active expiry" {
        r FLUSHALL
        set initial_expired [info_field [r info stats] expired_fields]

        r HSET myhash f1 v1
        assert_equal 1 [r HLEN myhash]
        assert_equal 0 [get_keys_with_volatile_items r]
        
        # Set initial long expiry
        r HEXPIRE myhash [get_long_expire_value HEXPIRE] FIELDS 1 f1
        assert_morethan [r HTTL myhash FIELDS 1 f1] 5000
        
        # Use HGETEX to set shorter expiry
        assert_equal "v1" [r HGETEX myhash PX 100 FIELDS 1 f1]
        
        # Wait for active expiry with new shorter time
        wait_for_active_expiry r myhash 0 $initial_expired 1
        
        assert_equal 0 [r EXISTS myhash]
        assert_equal 0 [get_keys_with_volatile_items r]
    }

    ##### HGETEX Active Expiry Keyspace Notifications #####
    foreach command {EX PX EXAT PXAT} {
        test "HGETEX $command keyspace notifications for active expiry" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]

            r HSET myhash f1 v1 f2 v2
            assert_equal 2 [r HLEN myhash]
            assert_equal 0 [get_keys_with_volatile_items r]

            assert_equal 2 [r HLEN myhash]
            set rd [setup_single_keyspace_notification r]
            
            # Set expiry with HGETEX
            r HGETEX myhash $command [get_short_expire_value $command] FIELDS 1 f1
            
            wait_for_active_expiry r myhash 1 $initial_expired 1
            assert_keyevent_patterns $rd myhash hexpire hexpired
            assert_equal 0 [get_keys_with_volatile_items r]
            $rd close
        }
    }
    
    test "HGETEX keyspace notification when key deleted with active expiry" {
        r FLUSHALL
        set initial_expired [info_field [r info stats] expired_fields]

        r HSET myhash f1 v1
        assert_equal 1 [r HLEN myhash]
        assert_equal 0 [get_keys_with_volatile_items r]
        
        set rd [setup_single_keyspace_notification r]
        
        # Set expiry on only field
        r HGETEX myhash PX [get_short_expire_value PX] FIELDS 1 f1
        
        wait_for_active_expiry r myhash 0 $initial_expired 1
        assert_equal 0 [r EXISTS myhash]
        # Should get both hexpired and del notifications
        assert_keyevent_patterns $rd myhash hexpire hexpired del
        assert_equal 0 [get_keys_with_volatile_items r]
        $rd close
    }

    ##### HSETEX Active Expiry Tests #####
    foreach command {EX PX EXAT PXAT} {
        test "HSETEX $command single field expires leaving other fields intact" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]
            r HSET myhash f2 v2
            assert_equal 1 [r HLEN myhash]
            assert_equal 0 [get_keys_with_volatile_items r]
            # Use HSETEX to set expiry
            r HSETEX myhash $command [get_short_expire_value $command] FIELDS 1 f1 v1
            wait_for_active_expiry r myhash 1 $initial_expired 1
            assert_equal 0 [get_keys_with_volatile_items r]
            assert_equal "{} v2" [r HGETEX myhash FIELDS 2 f1 f2]
        }

        test "HSETEX $command multiple fields expire leaving non-expired fields intact" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]
            r HSET myhash f2 v2
            assert_equal 1 [r HLEN myhash]
            assert_equal 0 [get_keys_with_volatile_items r]
            # Set expiry on multiple fields with HSETEX
            r HSETEX myhash $command [get_short_expire_value $command] FIELDS 2 f1 v1 f3 v3
            wait_for_active_expiry r myhash 1 $initial_expired 2
            assert_equal 0 [get_keys_with_volatile_items r]
            # Verify only non-expired field remains
            assert_equal "{} v2 {}" [r HGETEX myhash FIELDS 3 f1 f2 f3]
        }

        test "HSETEX $command hash key deleted when all fields expire" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]
            r HSETEX myhash $command [get_short_expire_value $command] FIELDS 1 f1 v1
            wait_for_active_expiry r myhash 0 $initial_expired 1
            assert_equal 0 [r EXISTS myhash]
        }

        test "HSETEX $command after HSETEX $command" {
            r FLUSHALL
            r HSETEX myhash EX 1000000000 FIELDS 1 f1 v1
            r HSETEX myhash PX 10 FIELDS 1 f2 v2
        }
    }

    test "HPERSIST cancels HSETEX expiry preventing field deletion" {
        r FLUSHALL
        r HSET myhash f2 v2
        assert_equal 1 [r HLEN myhash]
        # Set short expiry
        r HSETEX myhash PX [get_short_expire_value PX] FIELDS 1 f1 v1
        # Immediately persist to prevent expiry
        r HPERSIST myhash FIELDS 1 f1
        assert_equal -1 [r HTTL myhash FIELDS 1 f1]
        # Wait longer than original expiry time
        after 200
        # Field should still exist due to PERSIST
        assert_equal "v1" [r HGET myhash f1]
        assert_equal 2 [r HLEN myhash]
    }

    test "HSETEX overwrites existing field expiry with new shorter expiry" {
        r FLUSHALL
        set initial_expired [info_field [r info stats] expired_fields]
        r HSET myhash f1 v1
        assert_equal 1 [r HLEN myhash]
        assert_equal 0 [get_keys_with_volatile_items r]
        # Set initial long expiry
        r HEXPIRE myhash [get_long_expire_value HEXPIRE] FIELDS 1 f1
        assert_equal 1 [get_keys_with_volatile_items r]
        assert_morethan [r HTTL myhash FIELDS 1 f1] 5000
        # Use HSETEX to set shorter expiry
        r HSETEX myhash PX 100 FIELDS 1 f1 v1
        # Wait for active expiry with new shorter time
        wait_for_active_expiry r myhash 0 $initial_expired 1
        assert_equal 0 [get_keys_with_volatile_items r]
        assert_equal 0 [r EXISTS myhash]
    }

    ##### HSETEX Active Expiry Keyspace Notifications #####
    foreach command {EX PX EXAT PXAT} {
        test "HSETEX $command - keyspace notifications fired on field expiry" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]
            r HSET myhash f2 v2
            assert_equal 1 [r HLEN myhash]
            assert_equal 0 [get_keys_with_volatile_items r]
            set rd [setup_single_keyspace_notification r]
            r HSETEX myhash $command [get_short_expire_value $command] FIELDS 1 f1 v1
            wait_for_active_expiry r myhash 1 $initial_expired 1
            assert_keyevent_patterns $rd myhash hset hexpire hexpired
            assert_equal 0 [get_keys_with_volatile_items r]
            $rd close
        }
    }
    
    test "HSETEX - keyspace notifications include del event when hash key removed" {
        r FLUSHALL
        set initial_expired [info_field [r info stats] expired_fields]
        set rd [setup_single_keyspace_notification r]
        r HSETEX myhash PX 100 FIELDS 1 f1 v1
        wait_for_active_expiry r myhash 0 $initial_expired 1
        assert_equal 0 [r EXISTS myhash]
        assert_keyevent_patterns $rd myhash hset hexpire hexpired del
        $rd close
    }

    ##### Active expiry test with 1 node #####
    set rd [valkey_deferring_client]
    assert_equal {1} [psubscribe $rd __keyevent@*]

    test {Active expiry deletes entire key when only field expires} {
        r FLUSHALL
        set initial_expired [info_field [r info stats] expired_fields]
        r HSET myhash f1 v1
        assert_equal 1 [r HLEN myhash]
        assert_equal 1 [get_keys r]
        assert_equal 0 [get_keys_with_volatile_items r]
        r HPEXPIRE myhash 100 FIELDS 1 f1
        wait_for_active_expiry r myhash 0 $initial_expired 1
        # Key is deleted after its only field got expired
        assert_equal 0 [get_keys r]
        assert_equal 0 [get_keys_with_volatile_items r]
        assert_equal "" [r HGET myhash f1]
        assert_equal 0 [r EXISTS myhash]
        # Verify keyspace notifications
        assert_keyevent_patterns $rd myhash hset hexpire hexpired del
    }

    test {Active expiry removes only expired field while preserving others} {
        r FLUSHALL
        set initial_expired [info_field [r info stats] expired_fields]
        r HSET myhash f1 v1 f2 v2 f3 v3
        assert_equal 3 [r HLEN myhash]
        assert_equal 1 [get_keys r]
        assert_equal 0 [get_keys_with_volatile_items r]
        r HPEXPIRE myhash 100 FIELDS 1 f1
        set mem_before [r MEMORY USAGE myhash]
        wait_for_active_expiry r myhash 2 $initial_expired 1
        # Key still exists because it has 2 fields remaining
        assert_equal 1 [get_keys r]
        assert_equal 0 [get_keys_with_volatile_items r]
        assert_equal "{} v2 v3" [r HGETEX myhash FIELDS 3 f1 f2 f3]
        # Verify memory decreased after field expiry
        set mem_after [r MEMORY USAGE myhash]
        assert_morethan $mem_before $mem_after
        # Verify keyspace notifications
        assert_keyevent_patterns $rd myhash hset hexpire hexpired
        assert_equal 0 [get_keys_with_volatile_items r]
    }

    test {Active expiry reclaims memory correctly with large hash containing many fields} {
        r FLUSHALL
        set initial_expired [info_field [r info stats] expired_fields]
        set value [string repeat x 1024]
        set num_fields 10000
        # Set multiple fields
        for {set i 1} {$i <= $num_fields} {incr i} {
            lappend pairs "f$i" $value$i
        }
        r HSET myhash {*}$pairs
        assert_equal 0 [get_keys_with_volatile_items r]
        assert_equal $num_fields [r HLEN myhash]

        set mem_before_expire [r MEMORY USAGE myhash]
        if {$mem_before_expire eq ""} {set mem_before_expire 0}
        assert_morethan $mem_before_expire 10000000
        assert_equal 1 [get_keys r]
        assert_equal $num_fields [r HLEN myhash]
        r HPEXPIRE myhash 100 FIELDS 1 f1

        wait_for_active_expiry r myhash [expr {$num_fields - 1}] $initial_expired 1
        # Key still exists because it has num_fields 1 fields remaining
        assert_equal 1 [get_keys r]
        assert_equal "" [r HGET myhash f1]
        for {set i 2} {$i <= $num_fields} {incr i} {
            assert_equal $value$i [r HGET myhash "f$i"]
        }
        assert_equal 0 [get_keys_with_volatile_items r]

        # Expire all remaining fields
        set all_field_names {}
        for {set i 2} {$i <= $num_fields} {incr i} {
            lappend all_field_names "f$i"
        }
        r HPEXPIRE myhash 100 FIELDS [expr {$num_fields - 1}] {*}$all_field_names
        wait_for_active_expiry r myhash 0 $initial_expired $num_fields 350 100
        # Verify memory decreased by at least 15MB (size of hash key)
        set mem_after_expire [r MEMORY USAGE myhash]
        if {$mem_after_expire eq ""} {set mem_after_expire 0}
        assert_morethan [expr {$mem_before_expire - $mem_after_expire}] 10000000
        # Verify keyspace notifications
        assert_keyevent_patterns $rd myhash hset hexpire hexpired hexpire hexpired
        # Wait for del, maximum num_fields reads
        for {set i 2} {$i <= $num_fields} {incr i} {
            if {[string match "pmessage __keyevent@* __keyevent@*:del myhash" [$rd read]]} {
                break
            }
        }
        assert_equal 0 [get_keys_with_volatile_items r]
    }

    test {Active expiry handles fields with different TTL values correctly} {
        r FLUSHALL
        set initial_expired [info_field [r info stats] expired_fields]

        r HSET myhash f1 v1 f2 v2 f3 v3
        assert_equal 3 [r HLEN myhash]

        # Set very short expiry and longer expiry
        r HPEXPIRE myhash [get_short_expire_value HPEXPIRE] FIELDS 1 f1
        # Wait for f1 to expire
        wait_for_active_expiry r myhash 2 $initial_expired 1
        r HEXPIRE myhash [get_long_expire_value HEXPIRE] FIELDS 1 f2
        # f3 has no expiry
        # Verify f2 and f3 still exist
        assert_equal 2 [r HLEN myhash]
        assert_equal "{} v2 v3" [r HGETEX myhash FIELDS 3 f1 f2 f3]
        assert_keyevent_patterns $rd myhash hset hexpire hexpired hexpire
    }

    test {Active expiry removes only specified fields leaving others intact} {
        r FLUSHALL
        set initial_expired [info_field [r info stats] expired_fields]

        r HSET myhash f1 v1 f2 v2 f3 v3 f4 v4 f5 v5
        assert_equal 5 [r HLEN myhash]

        # Set expiry on alternating fields
        r HPEXPIRE myhash 100 FIELDS 2 f1 f3
        # f2, f4, f5 have no expiry

        wait_for_active_expiry r myhash 3 $initial_expired 2

        # Verify expired fields are gone and non-expired exists
        assert_equal "{} v2 {} v4 v5" [r HGETEX myhash FIELDS 5 f1 f2 f3 f4 f5]

        # Key should still exist
        assert_equal 1 [get_keys r]
    }

    $rd close

    test {Field TTL is removed when field value is overwritten with HSET} {
        r FLUSHALL
        r HSET myhash f1 v1
        r HEXPIRE myhash 100000 FIELDS 1 f1
        r HSET myhash f1 v2
        # TTL should be removed after overwrite
        assert_equal -1 [r HPTTL myhash FIELDS 1 f1]
        # Field should still exist
        assert_equal "v2" [r HGET myhash f1]
    }

    # Active expiry with field deletion and recreation
    test {Field TTL is cleared when field is deleted and recreated} {
        r FLUSHALL
        r HSET myhash f1 v1
        r HPEXPIRE myhash 100 FIELDS 1 f1
        r HDEL myhash f1
        r HSET myhash f1 v2
        assert_equal -1 [r HPTTL myhash FIELDS 1 f1]
        after 200
        assert_equal v2 [r HGET myhash f1]
    }

    ##### Test Active Expiry Tests with all hash expire commands #####
    foreach command {HEXPIRE HPEXPIRE HEXPIREAT HPEXPIREAT} {
        test "$command active expiry on single field" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]

            r HSET myhash f1 v1 f2 v2
            assert_equal 2 [r HLEN myhash]
            
            # Set expiry based on command type
            r $command myhash [get_short_expire_value $command] FIELDS 1 f1
            
            # Wait for active expiry
            wait_for_active_expiry r myhash 1 $initial_expired 1
            
            # Verify only expired field is gone
            assert_equal "{} v2" [r HGETEX myhash FIELDS 2 f1 f2]
        }

        test "$command active expiry with multiple fields" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]

            r HSET myhash f1 v1 f2 v2 f3 v3 f4 v4
            assert_equal 4 [r HLEN myhash]
            
            # Set expiry on multiple fields
            r $command myhash [get_short_expire_value $command] FIELDS 3 f1 f2 f4
            
            # Wait for active expiry
            wait_for_active_expiry r myhash 1 $initial_expired 3
            
            # Only f3 should remain
            assert_equal "{} {} v3 {}" [r HGETEX myhash FIELDS 4 f1 f2 f3 f4]
        }

        test "$command active expiry removes entire key when last field expires" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]

            r HSET myhash f1 v1
            assert_equal 1 [r HLEN myhash]
            
            # Set expiry on only field
            r $command myhash [get_short_expire_value $command] FIELDS 1 f1
            
            
            # Wait for active expiry to remove key
            wait_for_active_expiry r myhash 0 $initial_expired 1
            
            assert_equal 0 [r EXISTS myhash]
        }

        test "$command active expiry with non-existing fields" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]

            r HSET myhash f1 v1 f2 v2
            assert_equal 2 [r HLEN myhash]
            
            # Try to expire non-existing fields
            r $command myhash [get_short_expire_value $command] FIELDS 2 f3 f4
            
            
            # Wait to ensure no active expiry occurs
            after 1500
            assert [check_myhash_and_expired_subkeys r myhash 2 $initial_expired 0]
        }

        test "$command active expiry with mixed existing and non-existing fields" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]
            
            r HSET myhash f1 v1 f2 v2 f3 v3
            assert_equal 3 [r HLEN myhash]
            
            # Mix of existing and non-existing fields
            r $command myhash [get_short_expire_value $command] FIELDS 4 f1 f4 f3 f5
            
            
            # Wait for active expiry of existing fields only
            wait_for_active_expiry r myhash 1 $initial_expired 2
            
            # Only f2 should remain
            assert_equal "{} v2 {}" [r HGETEX myhash FIELDS 3 f1 f2 f3]
        }

        test "$command active expiry with already expired fields" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]

            r HSET myhash f1 v1 f2 v2 f3 v3
            assert_equal 3 [r HLEN myhash]
            
            # Set very short expiry on f1
            r $command myhash [get_short_expire_value $command] FIELDS 1 f1
            
            
            # Wait for active expiry
            wait_for_active_expiry r myhash 2 $initial_expired 1
            
            # Now try to expire f1 again (already expired) and f2 (existing)
            r $command myhash [get_short_expire_value $command] FIELDS 2 f1 f2
            
            # Wait for f2 to expire
            wait_for_active_expiry r myhash 1 $initial_expired 2
            
            # Only f3 should remain
            assert_equal "{} {} v3" [r HGETEX myhash FIELDS 3 f1 f2 f3]
        }
    }

    test "CLIENT PAUSE WRITE blocks hash field active expiry until pause ends" {
        r FLUSHALL
        set initial_expired [info_field [r info stats] expired_fields]

        r HSET myhash f1 v1 f2 v2
        assert_equal 2 [r HLEN myhash]

        # To avoid flakiness - run commands in transaction
        r multi
        
        r HPEXPIRE myhash 500 FIELDS 1 f1
        r CLIENT PAUSE 1200 WRITE
        
        r exec
        
        # Verify no expiry happened immediately after transaction
        assert_equal 2 [r HLEN myhash]
        assert_equal 0 [expr {[info_field [r info stats] expired_fields] - $initial_expired}]
        
        # Wait longer than expiry time while paused
        after 600
        
        # Field should still exist because active expiry is paused
        assert_equal 2 [r HLEN myhash]
        assert_equal 0 [expr {[info_field [r info stats] expired_fields] - $initial_expired}]
        
        # Wait for pause to end
        after 600
        
        # Now active expiry should work
        wait_for_active_expiry r myhash 1 $initial_expired 1 50 20
        
        assert_equal "{} v2" [r HMGET myhash f1 f2]
    }

    ##### Active Expiry Tests After RENAME/COPY/RESTORE Operations #####
    foreach command {HEXPIRE HPEXPIRE HEXPIREAT HPEXPIREAT} {
        foreach op {RENAME COPY RESTORE MOVE} {
            test "$command active expiry works correctly after $op operation" {
                r FLUSHALL
                r SELECT 0
                set initial_expired [info_field [r info stats] expired_fields]

                r HSET myhash f1 v1 f2 v2 f3 v3 f4 v4
                assert_equal 4 [r HLEN myhash]
                assert_equal 0 [get_keys_with_volatile_items r]

                # Set expiry on fields
                r $command myhash [get_short_expire_value $command] FIELDS 1 f1
                wait_for_active_expiry r myhash 3 $initial_expired 1
                r $command myhash [get_long_expire_value $command] FIELDS 1 f4
                assert_equal 1 [get_keys_with_volatile_items r]

                # Run op command
                if {$op eq "RENAME"} {
                    r RENAME myhash newhash
                    set target_key newhash
                } elseif {$op eq "COPY"} {
                    r COPY myhash copyhash
                    set target_key copyhash
                } elseif {$op eq "RESTORE"} {
                    # RESTORE
                    set serialized [r DUMP myhash]
                    r DEL myhash
                    r RESTORE restorehash 0 $serialized
                    set target_key restorehash
                } else {
                    r MOVE myhash 1
                    # Switch to target DB
                    r SELECT 1
                    set target_key myhash
                }
                if {$op eq "COPY"} {
                    assert_equal 2 [get_keys_with_volatile_items r]
                } else {
                    assert_equal 1 [get_keys_with_volatile_items r]
                }

                # Set expiry on fields after op command
                r $command $target_key [get_short_expire_value $command] FIELDS 1 f3
                # Wait for active expiry on "new" key
                wait_for_active_expiry r $target_key 2 $initial_expired 2
                
                assert_equal "{} v2 {}" [r HMGET $target_key f1 f2 f3]
                # In copy verify original hash hasnt changed
                if {$op eq "COPY"} {
                    assert_equal "{} v2 v3" [r HMGET myhash f1 f2 f3]
                }
            }
        }
    }

    foreach command {HEXPIRE HPEXPIRE HEXPIREAT HPEXPIREAT} {
        test "$command active expiry processes multiple hash keys with different field counts" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]
            
            # Create multiple hash keys
            for {set i 1} {$i <= 5} {incr i} {
                r HSET hash$i f1 v1_$i f2 v2_$i f3 v3_$i
            }
            assert_equal 5 [get_keys r]
            assert_equal 0 [get_keys_with_volatile_items r]
            
            r $command hash1 [get_short_expire_value $command] FIELDS 1 f1
            r $command hash2 [get_short_expire_value $command] FIELDS 2 f1 f2
            r $command hash3 [get_short_expire_value $command] FIELDS 3 f1 f2 f3
            r $command hash4 [get_short_expire_value $command] FIELDS 1 f2
            
            wait_for_condition 100 100 {
                [r HLEN hash1] eq 2 && [r HLEN hash2] eq 1 &&
                [r HLEN hash3] eq 0 && [r HLEN hash4] eq 2 && [r HLEN hash5] eq 3 &&
                [expr {[info_field [r info stats] expired_fields] - $initial_expired}] eq 7
            } else {
                fail "Fields should expire across multiple keys"
            }
            
            assert_equal "{} v2_1 v3_1" [r HMGET hash1 f1 f2 f3]
            assert_equal "{} {} v3_2" [r HMGET hash2 f1 f2 f3]
            assert_equal 0 [r EXISTS hash3]
            assert_equal "v1_4 {} v3_4" [r HMGET hash4 f1 f2 f3]
            assert_equal "v1_5 v2_5 v3_5" [r HMGET hash5 f1 f2 f3]
            assert_equal 4 [get_keys r]
            assert_equal 0 [get_keys_with_volatile_items r]

            # Set long expire
            r $command hash1 [get_long_expire_value $command] FIELDS 1 f2
            assert_equal 1 [get_keys_with_volatile_items r]
            
            r $command hash2 [get_long_expire_value $command] FIELDS 1 f3
            assert_equal 2 [get_keys_with_volatile_items r]
        }
    }

    foreach command {HEXPIRE HPEXPIRE HEXPIREAT HPEXPIREAT} {
        test "$command handles mixed short and long expiry times across multiple keys" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]
            
            r HSET key1 f1 v1 f2 v2 f3 v3
            r HSET key2 f1 v1 f2 v2 f3 v3
            r HSET key3 f1 v1 f2 v2 f3 v3
            r HSET key4 f1 v1 f2 v2 f3 v3
            assert_equal 4 [get_keys r]
            assert_equal 0 [get_keys_with_volatile_items r]
            
            r $command key2 [get_long_expire_value $command] FIELDS 1 f1
            assert_equal 1 [get_keys_with_volatile_items r]

            set short_expire [get_short_expire_value $command]
            r $command key1 $short_expire FIELDS 1 f1
            r $command key3 $short_expire FIELDS 2 f1 f2
            r $command key4 $short_expire FIELDS 3 f1 f2 f3
            
            wait_for_condition 100 100 {
                [r HLEN key1] eq 2 && [r HLEN key3] eq 1 &&
                [r HLEN key4] eq 0 && [expr {[info_field [r info stats] expired_fields] - $initial_expired}] eq 6
            } else {
                fail "Short expiry fields should expire"
            }
            
            assert_equal "{} v2 v3" [r HMGET key1 f1 f2 f3]
            assert_equal "v1 v2 v3" [r HMGET key2 f1 f2 f3]
            assert_equal "{} {} v3" [r HMGET key3 f1 f2 f3]
            assert_equal 0 [r EXISTS key4]
            assert_equal 3 [get_keys r]
            assert_equal 1 [get_keys_with_volatile_items r]
            
            assert_morethan [r HTTL key2 FIELDS 1 f1] 3000
        }
    }

    foreach command {HEXPIRE HPEXPIRE HEXPIREAT HPEXPIREAT} {
        test "$command deletes entire keys when all fields expire while preserving partial keys" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]
            
            # Create keys where some will be completely deleted
            for {set i 1} {$i <= 4} {incr i} {
                r HSET delkey$i f1 v1
            }
            r HSET keepkey f1 v1 f2 v2
            
            # Set expiry on f1 field in delkey1-4 (which is all the fields there)
            for {set i 1} {$i <= 4} {incr i} {
                r $command delkey$i [get_short_expire_value $command] FIELDS 1 f1
            }
            r $command keepkey [get_short_expire_value $command] FIELDS 1 f1
            
            # Wait for active expiry - 4 keys deleted, 1 key reduced
            wait_for_condition 100 100 {
                [r EXISTS delkey1] eq 0 && [r EXISTS delkey2] eq 0 &&
                [r EXISTS delkey3] eq 0 && [r EXISTS delkey4] eq 0 &&
                [r HLEN keepkey] eq 1 &&
                [info_field [r info stats] expired_fields] eq [expr {$initial_expired + 5}]
            } else {
                fail "Keys should be deleted when last field expires"
            }
            
            assert_equal "{} v2" [r HMGET keepkey f1 f2]
        }
    }

    foreach command {HEXPIRE HPEXPIRE HEXPIREAT HPEXPIREAT} {
        test "$command active expiry reclaims memory efficiently across multiple large hash keys" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]
            
            # Create keys with large values
            set large_value [string repeat "x" 1024]
            # 5 keys, 10 "large" fields in each
            for {set i 1} {$i <= 5} {incr i} {
                for {set j 1} {$j <= 10} {incr j} {
                    r HSET myhash$i f$j $large_value$i$j
                }
            }
            
            # Save initial memory
            set total_mem_before 0
            for {set i 1} {$i <= 5} {incr i} {
                set mem [r MEMORY USAGE myhash$i]
                if {$mem eq ""} {set mem 0}
                incr total_mem_before $mem
            }
            
            # For each key, set expire for 5 fields
            for {set i 1} {$i <= 5} {incr i} {
                assert_equal {1 1 1 1 1} [r $command myhash$i [get_short_expire_value $command] FIELDS 5 f1 f2 f3 f4 f5]
            }
            
            # Wait for expiry
            wait_for_condition 100 100 {
                [r HLEN myhash1] eq 5 && [r HLEN myhash2] eq 5 && 
                [r HLEN myhash3] eq 5 && [r HLEN myhash4] eq 5 &&
                [r HLEN myhash5] eq 5 &&
                [info_field [r info stats] expired_fields] eq [expr {$initial_expired + 25}]
            } else {
                fail "25 fields should expire across 5 keys"
            }
            
            # Verify memory reduction
            set total_mem_after 0
            for {set i 1} {$i <= 5} {incr i} {
                set mem [r MEMORY USAGE myhash$i]
                if {$mem eq ""} {set mem 0}
                incr total_mem_after $mem
            }
            
            # Memory should be reduced
            if {$total_mem_before > 0} {
                assert_morethan [expr {$total_mem_before - $total_mem_after}] 10000
            }
        }
    }

    ##### HINCRBY/HINCRBYFLOAT Active Expiry Tests #####
    foreach cmd {HINCRBY HINCRBYFLOAT} {
        # Set increment values
        if {$cmd eq "HINCRBY"} {
            set inc1 2
            set inc2 3
            set inc3 4
        } else {
            set inc1 2.5
            set inc2 3.5
            set inc3 4.5
        }

        # 1 key, 1 field
        test "$cmd recreates field with correct value after active expiry deletion" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]

            r HSET myhash f1 1
            assert_equal 1 [r HLEN myhash]

            # Set expiry on f1
            r HPEXPIRE myhash 100 FIELDS 1 f1

            # Wait for active expiry
            wait_for_active_expiry r myhash 0 $initial_expired 1

            # Try increment after expiry (should recreate field)
            r $cmd myhash f1 $inc1
            assert_equal $inc1 [r HGET myhash f1]
        }

        # 1 key, 1 field, increment before expiry
        test "$cmd preserves existing TTL when incrementing field value" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]

            r HSET myhash f1 1
            assert_equal 1 [r HLEN myhash]

            # Set expiry after increment
            r HEXPIRE myhash 100000 FIELDS 1 f1

            # Increment after expiry set
            r $cmd myhash f1 $inc1

            # Check value and expiry is still set
            assert_equal [expr {$inc1 + 1}] [r HGET myhash f1]
            assert_morethan [r HTTL myhash FIELDS 1 f1] 90000
        }

        # 1 key, 3 fields, increment multiple fields, expiry on multiple fields
        test "$cmd handles mix of expired and existing fields during increment operations" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]

            r HSET myhash f1 1 f2 2 f3 3
            assert_equal 3 [r HLEN myhash]

            # Set expiry on f1 and f3
            r HPEXPIRE myhash 100 FIELDS 2 f1 f3

            # Wait for active expiry
            wait_for_active_expiry r myhash 1 $initial_expired 2

            # Increment all fields (f1 and f3 should be recreated, f2 should increment)
            r $cmd myhash f1 $inc1
            r $cmd myhash f2 $inc2
            r $cmd myhash f3 $inc3

            # Check values
            assert_equal "$inc1 [expr {$inc2+2}] $inc3" [r HMGET myhash f1 f2 f3]
        }

        # 1 key, 3 fields, increment before expiry, then expire
        test "$cmd maintains TTL values when incrementing fields with existing expiry" {
            r FLUSHALL
            set initial_expired [info_field [r info stats] expired_fields]

            r HSET myhash f1 1 f2 2 f3 3
            assert_equal 3 [r HLEN myhash]

            # Set expiry on f1 and f3
            r HEXPIRE myhash 100000 FIELDS 2 f1 f3

            # Increment/ Decrement all fields
            r $cmd myhash f1 $inc1
            r $cmd myhash f3 -$inc3
            # Only f2 should remain
            assert_equal "[expr {$inc1+1}] 2 [expr {-$inc3+3}]" [r HMGET myhash f1 f2 f3]
        }
    }

    ### HDEL WITH ACTIVE EXPIRE #####
    test {HDEL removes both expired and non-expired fields deleting key when empty} {
        r FLUSHALL
        set initial_expired [info_field [r info stats] expired_fields]
        r HSET myhash f1 v1 f2 v2
        r HEXPIRE myhash 1 FIELDS 1 f1
        wait_for_active_expiry r myhash 1 $initial_expired 1
        # f1 is expired, f2 is not, f3 does not exist
        r HDEL myhash f1 f2 f3
        # f1 and f2 should be gone, f3 never existed
        assert_equal 0 [r HEXISTS myhash f1]
        assert_equal 0 [r HEXISTS myhash f2]
        assert_equal 0 [r HEXISTS myhash f3]
        # The key should be deleted since all fields are gone
        assert_equal 0 [r EXISTS myhash]
    }

    ##### HPERSIST TEST WITH ACTIVE EXPIRY #####
    test {HPERSIST returns -2 when attempting to persist already expired field} {
        r FLUSHALL
        set initial_expired [info_field [r info stats] expired_fields]
        r HSET myhash f1 v1
        r HPEXPIRE myhash 50 FIELDS 1 f1
        wait_for_active_expiry r myhash 0 $initial_expired 1
        assert_equal -2 [r HPERSIST myhash FIELDS 1 f1]
        assert_equal -2 [r HTTL myhash FIELDS 1 f1]
        assert_equal "" [r HGET myhash f1]
    }

    test {HPEXPIRE works correctly on field after HPERSIST removes its TTL} {
        r FLUSHALL
        set initial_expired [info_field [r info stats] expired_fields]
        r HSET myhash f1 v1
        r HEXPIRE myhash 10000 FIELDS 1 f1
        r HPERSIST myhash FIELDS 1 f1
        r HPEXPIRE myhash 150 FIELDS 1 f1
        wait_for_active_expiry r myhash 0 $initial_expired 1
        assert_equal 0 [r EXISTS myhash]
    }
}

##### Active expiry test slot migration #####
start_cluster 3 0 {tags {"cluster mytest external:skip"} overrides {cluster-node-timeout 1000}} {
    # Flush all data on all cluster nodes before starting
    for {set i 0} {$i < 3} {incr i} {
        R $i FLUSHALL
    }
    set R0_id [R 0 CLUSTER MYID]
    set R1_id [R 1 CLUSTER MYID]

    # Use a fixed hash tag to ensure key is in one slot
    set key "{mymigrate}myhash"

    test {Hash field TTL values and active expiry state preserved during cluster slot migration} {
        set initial_expired [info_field [R 0 info stats] expired_fields]
        
        R 0 HSET $key f1 v1 f2 v2 f3 v3
        assert_equal 3 [R 0 HLEN $key]

        set far_exp [expr {[clock seconds] + 30000}]
        R 0 HEXPIREAT $key $far_exp FIELDS 1 f1 ; # f1 with far expire
        R 0 HPEXPIRE $key 100 FIELDS 1 f2 ; # f2 with short expire
        assert_equal 1 [scan [lindex [regexp -inline {keys_with_volatile_items=([\d]+)} [R 0 info keyspace]] 1] "%d"]
        
        # Wait for short expire field (f2) to be expired by active expire
        wait_for_condition 100 100 {
            [R 0 HLEN $key] eq 2 &&
            [info_field [R 0 info stats] expired_fields] eq [expr {$initial_expired + 1}]
        } else {
            fail "Fields should have expired"
        }
        
        # Verify expired field returns empty string and non-expired returns value
        assert_equal "v1 {} v3" [R 0 HMGET $key f1 f2 f3]

        # Prepare slot migration
        set slot [R 0 CLUSTER KEYSLOT $key]
        assert_equal OK [R 1 CLUSTER SETSLOT $slot IMPORTING $R0_id]
        assert_equal OK [R 0 CLUSTER SETSLOT $slot MIGRATING $R1_id]

        # Migrate key to destination node
        R 0 MIGRATE [srv -1 host] [srv -1 port] $key 0 5000
        
        # Complete slot migration
        R 0 CLUSTER SETSLOT $slot NODE $R1_id
        R 1 CLUSTER SETSLOT $slot NODE $R1_id

        set initial_expired [info_field [R 1 info stats] expired_fields]
        
        # Verify after slot migration all fields are present and ttl is kept
        assert_match {1} [scan [regexp -inline {keys=([\d]*)} [R 1 info keyspace]] keys=%d]
        assert_equal 1 [scan [lindex [regexp -inline {keys_with_volatile_items=([\d]+)} [R 1 info keyspace]] 1] "%d"]
        assert_equal 2 [R 1 HLEN $key]
        assert_equal "v1 {} v3" [R 1 HMGET $key f1 f2 f3]
        assert_equal -1 [R 1 HTTL $key FIELDS 1 f3]
        assert_equal $far_exp [R 1 HEXPIRETIME $key FIELDS 1 f1]
        assert_equal -2 [R 1 HTTL $key FIELDS 1 f2]

        # Set short expiration on all fields (some do not exist)
        R 1 HPEXPIRE $key 100 FIELDS 3 f1 f2 f3
        
        # Verify active expiry
        wait_for_condition 200 50 {
            [R 1 HLEN $key] eq 0 &&
            [info_field [R 1 info stats] expired_fields] eq [expr {$initial_expired + 2}]
        } else {
            fail "All fields should have expired"
        }
        assert_match "" [scan [regexp -inline {keys=([\d]*)} [R 1 info keyspace]] keys=%d]
        # TODO handle empty #Keyspace properly
        # assert_equal 0 [scan [lindex [regexp -inline {keys_with_volatile_items=([\d]+)} [R 1 info keyspace]] 1] "%d"]
    }
}

##### Active expiry test slot migration with multiple fields #####
start_cluster 3 0 {tags {"cluster mytest external:skip"} overrides {cluster-node-timeout 1000}} {
    # Flush all data on all cluster nodes before starting
    for {set i 0} {$i < 3} {incr i} {
        R $i FLUSHALL
    }
    set R0_id [R 0 CLUSTER MYID]
    set R1_id [R 1 CLUSTER MYID]

    # Use a fixed hash tag to ensure key is in one slot
    set key "{mymigrate}myhash"

    test {Large hash with mixed TTL fields maintains expiry state after cluster slot migration} {
        set initial_expired [info_field [R 0 info stats] expired_fields]
        set num_fields 100

        # Create hash fields
        for {set i 1} {$i <= $num_fields} {incr i} {
            lappend pairs "f$i" "v$i"
        }
        R 0 HSET $key {*}$pairs
        assert_equal $num_fields [R 0 HLEN $key]

        set far_exp [expr {[clock seconds] + 30000}]
        # Set large TTL on 25 fields
        for {set i 1} {$i <= 25} {incr i} {
            R 0 HEXPIREAT $key $far_exp FIELDS 1 "f$i"
        }

        # Set short TTL on 25 fields
        for {set i 26} {$i <= 50} {incr i} {
            R 0 HPEXPIRE $key 100 FIELDS 1 "f$i"
        }
        
        # wait for short expire field to be expired by active expire
        wait_for_condition 100 100 {
            [R 0 HLEN $key] eq 75 &&
            [info_field [R 0 info stats] expired_fields] eq [expr {$initial_expired + 25}]
        } else {
            fail "Fields should have expired"
        }
        
        # Verify expired fields return empty string and non-expired return values
        for {set i 26} {$i <= 50} {incr i} {
            assert_equal "" [R 0 HGET $key "f$i"]
        }
        for {set i 1} {$i <= 25} {incr i} {
            assert_equal "v$i" [R 0 HGET $key "f$i"]
        }
        for {set i 51} {$i <= $num_fields} {incr i} {
            assert_equal "v$i" [R 0 HGET $key "f$i"]
        }

        # Prepare slot migration
        set slot [R 0 CLUSTER KEYSLOT $key]
        assert_equal OK [R 1 CLUSTER SETSLOT $slot IMPORTING $R0_id]
        assert_equal OK [R 0 CLUSTER SETSLOT $slot MIGRATING $R1_id]

        # Migrate key to destination node
        R 0 MIGRATE [srv -1 host] [srv -1 port] $key 0 5000
        
        # Complete slot migration
        R 0 CLUSTER SETSLOT $slot NODE $R1_id
        R 1 CLUSTER SETSLOT $slot NODE $R1_id
        
        set initial_expired [info_field [R 1 info stats] expired_fields]
        # Verify after slot migration all fields are present and ttl is kept
        assert_equal 75 [R 1 HLEN $key]
        for {set i 1} {$i <= $num_fields} {incr i} {
            if {$i > 50} {
                assert_equal -1 [R 1 HTTL $key FIELDS 1 "f$i"]
                assert_equal "v$i" [R 1 HGET $key "f$i"]
            } else {
                if {$i <= 25} {
                    assert_equal $far_exp [R 1 HEXPIRETIME $key FIELDS 1 f$i]
                    assert_equal "v$i" [R 1 HGET $key "f$i"]
                } else {
                    assert_equal -2 [R 1 HTTL $key FIELDS 1 "f$i"]
                    assert_equal "" [R 1 HGET $key "f$i"]
                }
            }
        }

        # Set short expiration on all fields (some do not exist)
        set fields {}
        for {set i 1} {$i <= 100} {incr i} {
            lappend fields "f$i"
        }
        R 1 HPEXPIRE $key 100 FIELDS 100 {*}$fields
        
        # Verify active expiry
        wait_for_condition 100 100 {
            [R 1 HLEN $key] eq 0 &&
            [info_field [R 1 info stats] expired_fields] eq [expr {$initial_expired + 75}]
        } else {
            fail "All fields should have expired"
        }
    }
}

##### Active expiry test replication #####
start_server {tags {"hashexpire external:skip"}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    start_server {tags {needs:repl external:skip}} {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]
        # Set this inner layer server as replica
        set replica [srv 0 client]

        test {Hash field active expiry on primary triggers HDEL replication to replica} {
            lassign [setup_replication_test $primary $replica $primary_host $primary_port] primary_initial_expired replica_initial_expired

            # Initialize deferred clients and subscribe to keyspace notifications
            foreach instance [list $primary $replica] {
                $instance config set notify-keyspace-events KEA
            }
            set rd_primary [valkey_deferring_client -1]
            set rd_replica [valkey_deferring_client $replica_host $replica_port]
            foreach rd [list $rd_primary $rd_replica] {
                assert_equal {1} [psubscribe $rd __keyevent@*]
            }

            # Create hash and timing f1 < f2 expiry times
            set f1_exp [expr {[clock seconds] + 10000}]

            # Setup hash, set expire and set expire 0
            $primary HSET myhash f1 v1 f2 v2 ;# Should trigger hset
            wait_for_ofs_sync $primary $replica
            
            $primary HPEXPIRE myhash 500 FIELDS 1 f1 ;# Should trigger 1 hexpire and then hexpired (for primary) and 1 hdel (for replica)
            wait_for_ofs_sync $primary $replica
            
            # Wait for active expiry
            wait_for_active_expiry $primary myhash 1 $primary_initial_expired 1
            # Ensure the replica does not increment expired_fields
            assert_equal $replica_initial_expired [info_field [$replica info stats] expired_fields]
            
            # Verify expired field returns empty string and non-expired returns value
            foreach instance [list $primary $replica] {
                assert_equal "{} v2" [$instance HMGET myhash f1 f2]
                assert_equal 0 [get_keys_with_volatile_items $instance]
            }
            
            # Verify keyspace notification
            foreach rd [list $rd_primary $rd_replica] {
                assert_keyevent_patterns $rd myhash hset
                assert_keyevent_patterns $rd myhash hexpire
            }
            # primary gets hexpired and replica gets hdel
            assert_keyevent_patterns $rd_primary myhash hexpired
            assert_keyevent_patterns $rd_replica myhash hdel

            $rd_primary close
            $rd_replica close
        }

        start_server {tags {needs:repl external:skip}} {
            $primary FLUSHALL
            set replica_2 [srv 0 client]
            set replica_2_host [srv 0 host]
            set replica_2_port [srv 0 port]
            
            test {Hash field TTL and active expiry propagates correctly through chain replication} {
                $replica replicaof $primary_host $primary_port
                # Wait for R2 to connect to R1
                wait_for_condition 100 100 {
                    [info_field [$replica info replication] master_link_status] eq "up"
                } else {
                    fail "Replica <-> Primary connection not established"
                }

                $replica_2 replicaof $replica_host $replica_port
                # Wait for R2 to connect to R1
                wait_for_condition 100 100 {
                    [info_field [$replica info replication] master_link_status] eq "up"
                } else {
                    fail "Second replica <-> First replica connection not established"
                }

                # Initialize deferred clients and subscribe to keyspace notifications
                foreach instance [list $primary $replica $replica_2] {
                    $instance config set notify-keyspace-events KEA
                }
                set rd_primary [valkey_deferring_client -2]
                set rd_replica [valkey_deferring_client -1]
                set rd_replica_2 [valkey_deferring_client $replica_2_host $replica_2_port]
                foreach rd [list $rd_primary $rd_replica $rd_replica_2] {
                    assert_equal {1} [psubscribe $rd __keyevent@*]
                }
    
                # Create hash and timing f1 < f2 expiry times
                set f1_exp [expr {[clock seconds] + 10000}]

                ############################################# STEUP HASH #############################################
                $primary HSET myhash f1 v1 f2 v2 ;# Should trigger 3 hset
                $primary HEXPIREAT myhash $f1_exp FIELDS 1 f1 ;# Should trigger 3 hexpire
                wait_for_ofs_sync $primary $replica
                wait_for_ofs_sync $replica $replica_2

                set primary_initial_expired [info_field [$primary info stats] expired_fields]
                set replica_initial_expired [info_field [$replica info stats] expired_fields]
                set replica_2_initial_expired [info_field [$replica_2 info stats] expired_fields]
                
                $primary HPEXPIRE myhash 100 FIELDS 1 f1 ;# Should trigger 1 hexpired (for primary) and 2 hdel (for replicas)
                wait_for_ofs_sync $primary $replica
                wait_for_ofs_sync $replica $replica_2
                
                # Wait for active expire
                wait_for_active_expiry $primary myhash 1 $primary_initial_expired 1
                
                # Ensure the replica does not increment expired_fields
                assert_equal $replica_initial_expired [info_field [$replica info stats] expired_fields]
                assert_equal $replica_2_initial_expired [info_field [$replica_2 info stats] expired_fields]
            

                # Verify expired field returns empty string and non-expired returns value
                foreach instance [list $primary $replica $replica_2] {
                    assert_equal "{} v2" [$instance HMGET myhash f1 f2]
                    assert_equal 0 [get_keys_with_volatile_items $instance]
                }
                
                # primary gets hexpired and replicas get hdel
                foreach rd [list $rd_primary $rd_replica $rd_replica_2] {
                    assert_keyevent_patterns $rd myhash hset hexpire hexpire
                }
                assert_keyevent_patterns $rd_primary myhash hexpired
                assert_keyevent_patterns $rd_replica myhash hdel
                assert_keyevent_patterns $rd_replica_2 myhash hdel

                $rd_primary close
                $rd_replica close
                $rd_replica_2 close
            }
        }
    
        proc verify_values {instance f1_exp f2_exp} {
            assert_equal $f1_exp [$instance HEXPIRETIME myhash FIELDS 1 f1]
            assert_equal $f2_exp [$instance HEXPIRETIME myhash FIELDS 1 f2]
            assert_equal -1 [$instance HTTL myhash FIELDS 1 f3]
            assert_match {1} [scan [regexp -inline {keys=([\d]*)} [$instance info keyspace]] keys=%d]
            assert_equal "v1" [$instance HGET myhash f1]
            assert_equal "v2" [$instance HGET myhash f2]
            assert_equal "v3" [$instance HGET myhash f3]
            assert_equal 3 [$instance HLEN myhash]
        }

        test {Hash field TTL values remain intact after replica promotion to primary} {
            lassign [setup_replication_test $primary $replica $primary_host $primary_port] primary_initial_expired replica_initial_expired

            # Initialize deferred clients and subscribe to keyspace notifications
            foreach instance [list $primary $replica] {
                $instance config set notify-keyspace-events KEA
            }
            set rd_primary [valkey_deferring_client -1]
            set rd_replica [valkey_deferring_client $replica_host $replica_port]
            foreach rd [list $rd_primary $rd_replica] {
                assert_equal {1} [psubscribe $rd __keyevent@*]
            }
            
            # Create hash fields with TTL on primary
            set f1_exp [expr {[clock seconds] + 2000}]
            set f2_exp [expr {[clock seconds] + 300000}]
            $primary HSET myhash f1 v1 f2 v2 f3 v3
            $primary HEXPIREAT myhash $f1_exp FIELDS 1 f1
            $primary HEXPIREAT myhash $f2_exp FIELDS 1 f2
            # f3 remains persistent

            # Wait for full sync
            wait_for_ofs_sync $primary $replica

            # Verify primary and replica are the same
            foreach instance [list $primary $replica] {
                verify_values $instance $f1_exp $f2_exp
                assert_equal 1 [get_keys_with_volatile_items $instance]
            }

            # Perform failover
            $replica replicaof no one
            # Wait for replica to become primary
            wait_for_condition 100 100 {
                [info_field [$replica info replication] role] eq "master"
            } else {
                fail "Replica didn't become master"
            }

            # Check all values that checked before are the same
            verify_values $replica $f1_exp $f2_exp
            
            # Set f1 to expire in 1 second and wait for active expiration
            set replica_initial_expired [info_field [$replica info stats] expired_fields]
            $replica HEXPIRE myhash 1 FIELDS 1 f1
            wait_for_active_expiry $replica myhash 2 $replica_initial_expired 1

            assert_equal "{} v2 v3" [$replica HMGET myhash f1 f2 f3]
            # Not affected primary
            assert_equal 3 [$primary HLEN myhash]
            assert_equal "v1 v2 v3" [$primary HMGET myhash f1 f2 f3]
            set primary_initial_expired [info_field [$primary info stats] expired_fields]
            assert_equal 0 [expr {[info_field [$primary info stats] expired_fields] - $primary_initial_expired}]

            foreach rd [list $rd_primary $rd_replica] {
                assert_keyevent_patterns $rd myhash hset hexpire hexpire
            }
            assert_keyevent_patterns $rd_replica myhash hexpire
            assert_keyevent_patterns $rd_replica myhash hexpired
            $rd_primary close
            $rd_replica close
        }

        test {Hash field TTL values persist correctly during FAILOVER command execution} {
            lassign [setup_replication_test $primary $replica $primary_host $primary_port] primary_initial_expired replica_initial_expired

            # Initialize deferred clients and subscribe to keyspace notifications
            foreach instance [list $primary $replica] {
                $instance config set notify-keyspace-events KEA
            }
            set rd_primary [valkey_deferring_client -1]
            set rd_replica [valkey_deferring_client $replica_host $replica_port]
            foreach rd [list $rd_primary $rd_replica] {
                assert_equal {1} [psubscribe $rd __keyevent@*]
            }
            
            # Create hash fields with TTL on primary
            set f1_exp [expr {[clock seconds] + 2000}]
            set f2_exp [expr {[clock seconds] + 300000}]
            $primary HSET myhash f1 v1 f2 v2 f3 v3
            $primary HEXPIREAT myhash $f1_exp FIELDS 1 f1
            $primary HEXPIREAT myhash $f2_exp FIELDS 1 f2
            # f3 remains persistent

            # Wait for full sync
            wait_for_ofs_sync $primary $replica

            # Verify primary and replica are the same
            foreach instance [list $primary $replica] {
                verify_values $instance $f1_exp $f2_exp
                assert_equal 1 [get_keys_with_volatile_items $instance]
            }

            # Perform failover swap roles
            $primary FAILOVER TO $replica_host $replica_port
            # Wait for role swap
            wait_for_condition 100 100 {
                [info_field [$replica info replication] role] eq "master" &&
                [info_field [$primary info replication] role] eq "slave"
            } else {
                fail "Failover didn't complete"
            }

            # Verify primary and replica are still the same
            foreach instance [list $primary $replica] {
                verify_values $instance $f1_exp $f2_exp
                assert_equal 1 [get_keys_with_volatile_items $instance]
            }
            
            # Set f1 to expire in 1 second and wait for active expiration
            $replica HEXPIRE myhash 1 FIELDS 1 f1 ;# will trigger hexpire
            wait_for_ofs_sync $replica $primary
            set replica_initial_expired [info_field [$replica info stats] expired_fields]
            wait_for_active_expiry $replica myhash 2 $replica_initial_expired 1

            # Verify prev primary, which is now replica of new primary (prev primary) is sync
            assert_equal 2 [$primary HLEN myhash]
            # Verify expiry
            assert_equal "{} v2 v3" [$replica HMGET myhash f1 f2 f3]
            assert_equal "" [$primary HGET myhash f1]
            assert_equal "v2" [$primary HGET myhash f2]
            assert_equal "v3" [$primary HGET myhash f3]

            # Primary is now replica, so no expected change in expired_fields
            assert_equal [info_field [$primary info stats] expired_fields] $primary_initial_expired

            foreach rd [list $rd_primary $rd_replica] {
                assert_keyevent_patterns $rd myhash hset hexpire hexpire hexpire
            }
            assert_keyevent_patterns $rd_replica myhash hexpired
            assert_keyevent_patterns $rd_primary myhash hdel
            $rd_primary close
            $rd_replica close
        }
    }
}

## Check monitor tests ###
start_server {tags {"hashexpire external:skip"}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    start_server {tags {needs:repl external:skip}} {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]
        # Set this inner layer server as replica
        set replica [srv 0 client]

        proc setup_replica_monitor_test {primary replica primary_host primary_port replica_host replica_port} {
            lassign [setup_replication_test $primary $replica $primary_host $primary_port] primary_initial_expired replica_initial_expired

            set rd_replica [valkey_deferring_client $replica_host $replica_port]
            $rd_replica monitor
            assert_match {*OK*} [$rd_replica read]
            
            return [list $primary_initial_expired $rd_replica]
        }
        
        proc read_monitor_output {rd_replica read_amount} {
            set res {}
            set i 0
            while {$i < $read_amount} {
                set curr_read [$rd_replica read]
                
                # Skip lines with INFO commands
                if {[regexp {\"info\"} $curr_read] || [regexp {\"SELECT\"} $curr_read]} {
                    continue
                }
                lappend res $curr_read
                incr i
            }
            $rd_replica close
            return [join $res " "]
        }

        # These tests are flaky, probably monitor output should be filtered 
        test {Multiple expired hash fields are replicated as single HDEL command to replica} {
            lassign [setup_replica_monitor_test $primary $replica $primary_host $primary_port $replica_host $replica_port] primary_initial_expired rd_replica
            $primary HSET myhash f1 v1 f2 v2 f3 v3
            wait_for_ofs_sync $primary $replica
            $primary HPEXPIRE myhash 50 FIELDS 1 f2
            wait_for_ofs_sync $primary $replica
            wait_for_active_expiry $primary myhash 2 $primary_initial_expired 1
            set _ [read_monitor_output $rd_replica 3]
        } {*HSET*myhash*f1*f2*f3*HDEL*myhash*f2*}

        test {HDEL replication includes only actually expired fields not non-existent ones} {
            lassign [setup_replica_monitor_test $primary $replica $primary_host $primary_port $replica_host $replica_port] primary_initial_expired rd_replica
            
            $primary HSET myhash f1 v1 f2 v2 f3 v3
            wait_for_ofs_sync $primary $replica
            $primary HPEXPIRE myhash 50 FIELDS 2 f1 f5
            wait_for_ofs_sync $primary $replica
            wait_for_active_expiry $primary myhash 2 $primary_initial_expired 1
            set _ [read_monitor_output $rd_replica 3]
        } {*HSET*myhash*f1*f2*f3*HDEL*myhash*f1*}
    }
}

start_server {tags {"hashexpire external:skip"}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    start_server {tags {needs:repl external:skip}} {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]

        test {expired_fields metric increments only on primary not replica during field expiry} {
            lassign [setup_replication_test $primary $replica $primary_host $primary_port] primary_initial_expired replica_initial_expired
            
            # Create hash fields with different TTLs
            $primary HSET myhash f1 v1 f2 v2 f3 v3 f4 v4
            $primary HEXPIRE myhash 3000 FIELDS 1 f1
            $primary HSETEX myhash EX 5000 FIELDS 1 f2 v2
            $primary HEXPIRE myhash 60000 FIELDS 1 f3
            wait_for_ofs_sync $primary $replica

            # Verify PERSIST
            assert_equal "v3" [$primary HGETEX myhash PERSIST FIELDS 1 f3]
            wait_for_ofs_sync $primary $replica
            assert_equal -1 [$primary HTTL myhash FIELDS 1 f3]
            assert_equal -1 [$replica HTTL myhash FIELDS 1 f3]

            $primary HPEXPIRE myhash 1 FIELDS 1 f1
            wait_for_ofs_sync $primary $replica
            # Wait for active expiry
            wait_for_active_expiry $primary myhash 3 $primary_initial_expired 1

            assert_equal 0 [info_field [$replica info stats] expired_fields]
        }

        test {expired_fields metric correctly tracks sequential field expirations in replication} {
            lassign [setup_replication_test $primary $replica $primary_host $primary_port] primary_initial_expired replica_initial_expired
            # Initialize deferred clients and subscribe to keyspace notifications
            foreach instance [list $primary $replica] {
                $instance config set notify-keyspace-events KEA
            }
            set rd_primary [valkey_deferring_client -1]
            set rd_replica [valkey_deferring_client $replica_host $replica_port]
            foreach rd [list $rd_primary $rd_replica] {
                assert_equal {1} [psubscribe $rd __keyevent@*]
            }
            
            # Create hash fields with different TTLs
            $primary HSET myhash f1 v1 f2 v2 f3 v3 f4 v4
            $primary HEXPIRE myhash 3000 FIELDS 1 f1
            $primary HSETEX myhash EX 5000 FIELDS 1 f2 v2
            $primary HEXPIRE myhash 60000 FIELDS 1 f3
            wait_for_ofs_sync $primary $replica

            # Verify TTLs are set correctly
            assert_morethan [$primary HTTL myhash FIELDS 1 f1] 0
            assert_morethan [$primary HTTL myhash FIELDS 1 f2] 0
            assert_morethan [$primary HTTL myhash FIELDS 1 f3] 0
            assert_equal -1 [$primary HTTL myhash FIELDS 1 f4]

            assert_equal 4 [$primary HLEN myhash]
            assert_equal 4 [$replica HLEN myhash]

            # Verify values
            assert_equal "v1 v2 v3 v4" [$primary HMGET myhash f1 f2 f3 f4]
            assert_equal "v1 v2 v3 v4" [$replica HMGET myhash f1 f2 f3 f4]

            # Verify PERSIST
            assert_equal "v3" [$primary HGETEX myhash PERSIST FIELDS 1 f3]
            wait_for_ofs_sync $primary $replica
            assert_equal -1 [$primary HTTL myhash FIELDS 1 f3]
            assert_equal -1 [$replica HTTL myhash FIELDS 1 f3]

            assert_equal 1 [get_keys_with_volatile_items $primary]
            assert_equal 1 [get_keys_with_volatile_items $replica]
            # Expire fields one by one
            for {set i 1} {$i <= 4} {incr i} {
                assert_equal 1 [get_keys $primary]
                assert_equal 1 [get_keys $replica]
                
                # Set field to expire immediately
                $primary HPEXPIRE myhash 1 FIELDS 1 f$i
                wait_for_ofs_sync $primary $replica
            
                # Wait for active expiry
                wait_for_active_expiry $primary myhash [expr {4 - $i}] $primary_initial_expired $i

                # Replica should NOT increment expired_fields
                assert_equal 0 [info_field [$replica info stats] expired_fields]
                
                # Replica should also have the field removed with replication
                assert_equal [expr {4 - $i}] [$replica HLEN myhash]
            }
            assert_equal 0 [get_keys_with_volatile_items $primary]
            assert_equal 0 [get_keys_with_volatile_items $replica]
            
            # Hash should be deleted when all fields expire
            assert_equal 0 [$primary EXISTS myhash]
            assert_equal 0 [$replica EXISTS myhash]
            assert_equal 0 [get_keys $primary]
            assert_equal 0 [get_keys $replica]
            assert_equal 0 [get_keys_with_volatile_items $primary]
            assert_equal 0 [get_keys_with_volatile_items $replica]
                
            foreach rd [list $rd_primary $rd_replica] {
                assert_keyevent_patterns $rd myhash hset hexpire hset hexpire hexpire hpersist hexpire
            }
            assert_keyevent_patterns $rd_primary myhash hexpired ; # f1
            assert_keyevent_patterns $rd_replica myhash hdel
            foreach rd [list $rd_primary $rd_replica] {
                assert_keyevent_patterns $rd myhash hexpire
            }
            assert_keyevent_patterns $rd_primary myhash hexpired ; # f2
            assert_keyevent_patterns $rd_replica myhash hdel
            foreach rd [list $rd_primary $rd_replica] {
                assert_keyevent_patterns $rd myhash hexpire
            }
            assert_keyevent_patterns $rd_primary myhash hexpired ; # f3
            assert_keyevent_patterns $rd_replica myhash hdel
            foreach rd [list $rd_primary $rd_replica] {
                assert_keyevent_patterns $rd myhash hexpire
            }
            assert_keyevent_patterns $rd_primary myhash hexpired del ; # f4
            assert_keyevent_patterns $rd_replica myhash hdel del
            $rd_primary close
            $rd_replica close
        }
    }
}
