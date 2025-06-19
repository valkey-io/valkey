
proc info_field {info field} {
foreach line [split $info "\n"] {
    if {[string match "$field:*" $line]} {
        return [string trim [lindex [split $line ":"] 1]]
    }
}
return ""
}

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

    # Field should still be present in memory due to lazy expiry
    assert_equal 1 [r HLEN myhash]

    # Overwrite with HINCRBY (no TTL) before accessing
    r HINCRBY myhash field1 1

    # TTL should now be gone; field becomes persistent
    set ttl [r HPTTL myhash FIELDS 1 field1]
    assert_equal -1 $ttl
    assert_equal 1 [r HGET myhash field1]
    assert_equal 1 [r HLEN myhash]

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



#### Replication

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

    }
}