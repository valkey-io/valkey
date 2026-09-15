start_server {tags {"incr"}} {
    test {INCR against nonexistent key} {
        set res {}
        append res [r incr novar]
        append res [r get novar]
    } {11}

    test {INCR against key created by incr itself} {
        r incr novar
    } {2}

    test {DECR against key created by incr} {
        r decr novar
    } {1}

    test {DECR against key is not exist and incr} {
        r del novar_not_exist
        assert_equal {-1} [r decr novar_not_exist]
        assert_equal {0} [r incr novar_not_exist]
    }

    test {INCR against key originally set with SET} {
        r set novar 100
        r incr novar
    } {101}

    test {INCR over 32bit value} {
        r set novar 17179869184
        r incr novar
    } {17179869185}

    test {INCRBY over 32bit value with over 32bit increment} {
        r set novar 17179869184
        r incrby novar 17179869184
    } {34359738368}

    test {INCR fails against key with spaces (left)} {
        r set novar "    11"
        catch {r incr novar} err
        format $err
    } {ERR*}

    test {INCR fails against key with spaces (right)} {
        r set novar "11    "
        catch {r incr novar} err
        format $err
    } {ERR*}

    test {INCR fails against key with spaces (both)} {
        r set novar "    11    "
        catch {r incr novar} err
        format $err
    } {ERR*}

    test {DECRBY negation overflow} {
        r set x 0
        catch {r decrby x -9223372036854775808} err
        format $err
    } {ERR*}

    test {INCR fails against a key holding a list} {
        r rpush mylist 1
        catch {r incr mylist} err
        r rpop mylist
        format $err
    } {WRONGTYPE*}

    test {DECRBY over 32bit value with over 32bit increment, negative res} {
        r set novar 17179869184
        r decrby novar 17179869185
    } {-1}

    test {DECRBY against key is not exist} {
        r del key_not_exist
        assert_equal {-1} [r decrby key_not_exist 1]
    }

    test {INCR can modify objects in-place} {
        r set foo 20000
        r incr foo
        assert_refcount 1 foo
        set old [lindex [split [r debug object foo]] 1]
        r incr foo
        set new [lindex [split [r debug object foo]] 1]
        assert {[string range $old 0 2] eq "at:"}
        assert {[string range $new 0 2] eq "at:"}
        assert {$old eq $new}
    } {} {needs:debug}

    test {INCRBYFLOAT against nonexistent key} {
        r del novar
        list    [roundFloat [r incrbyfloat novar 1]] \
                [roundFloat [r get novar]] \
                [roundFloat [r incrbyfloat novar 0.25]] \
                [roundFloat [r get novar]]
    } {1 1 1.25 1.25}

    test {INCRBYFLOAT against key originally set with SET} {
        r set novar 1.5
        roundFloat [r incrbyfloat novar 1.5]
    } {3}

    test {INCRBYFLOAT over 32bit value} {
        r set novar 17179869184
        r incrbyfloat novar 1.5
    } {17179869185.5}

    test {INCRBYFLOAT over 32bit value with over 32bit increment} {
        r set novar 17179869184
        r incrbyfloat novar 17179869184
    } {34359738368}

    test {INCRBYFLOAT fails against key with spaces (left)} {
        set err {}
        r set novar "    11"
        catch {r incrbyfloat novar 1.0} err
        format $err
    } {ERR *valid*}

    test {INCRBYFLOAT fails against key with spaces (right)} {
        set err {}
        r set novar "11    "
        catch {r incrbyfloat novar 1.0} err
        format $err
    } {ERR *valid*}

    test {INCRBYFLOAT fails against key with spaces (both)} {
        set err {}
        r set novar " 11 "
        catch {r incrbyfloat novar 1.0} err
        format $err
    } {ERR *valid*}

    test {INCRBYFLOAT fails against a key holding a list} {
        r del mylist
        set err {}
        r rpush mylist 1
        catch {r incrbyfloat mylist 1.0} err
        r del mylist
        format $err
    } {WRONGTYPE*}

    # On some platforms strtold("+inf") with valgrind returns a non-inf result
    test {INCRBYFLOAT does not allow NaN or Infinity} {
            r set foo 0
            set err {}
            catch {r incrbyfloat foo +inf} err
            set err
            # p.s. no way I can force NaN to test it from the API because
            # there is no way to increment / decrement by infinity nor to
            # perform divisions.
    } {ERR *would produce*} {valgrind:skip}

    test {INCRBYFLOAT decrement} {
        r set foo 1
        roundFloat [r incrbyfloat foo -1.1]
    } {-0.1}

    test {string to double with null terminator} {
        r set foo 1
        r setrange foo 2 2
        catch {r incrbyfloat foo 1} err
        format $err
    } {ERR *valid*}

    test {No negative zero} {
        r del foo
        r incrbyfloat foo [expr double(1)/41]
        r incrbyfloat foo [expr double(-1)/41]
        r get foo
    } {0}

    test {INCREX keyspace notifications} {
        set db [expr {$::singledb ? 0 : 9}]
        r config set notify-keyspace-events KEA
        set rd [valkey_deferring_client]
        assert_equal {1} [psubscribe $rd *]
        r del foo
        assert_equal "pmessage * __keyspace@${db}__:foo del" [$rd read]
        assert_equal "pmessage * __keyevent@${db}__:del foo" [$rd read]
        # Integer mode -> incrby
        r increx foo byint 5
        assert_equal "pmessage * __keyspace@${db}__:foo incrby" [$rd read]
        assert_equal "pmessage * __keyevent@${db}__:incrby foo" [$rd read]
        # Float mode -> incrbyfloat
        r increx foo byfloat 1
        assert_equal "pmessage * __keyspace@${db}__:foo incrbyfloat" [$rd read]
        assert_equal "pmessage * __keyevent@${db}__:incrbyfloat foo" [$rd read]
        # An expiry emits a second, separate event.
        r increx foo byint 1 ex 100
        assert_equal "pmessage * __keyspace@${db}__:foo incrby" [$rd read]
        assert_equal "pmessage * __keyevent@${db}__:incrby foo" [$rd read]
        assert_equal "pmessage * __keyspace@${db}__:foo expire" [$rd read]
        assert_equal "pmessage * __keyevent@${db}__:expire foo" [$rd read]
        $rd close
        r config set notify-keyspace-events ""
    }

    test {INCREX no negative zero} {
        r del foo
        r increx foo byfloat [expr double(1)/41]
        r increx foo byfloat [expr double(-1)/41]
        r get foo
    } {0}

    test {INCREX default increment is 1} {
        r del foo
        r increx foo
    } {1 1}

    test {INCREX BYINT increments by given amount} {
        r del foo
        r increx foo byint 5
        r increx foo byint 5
    } {10 5}

    test {INCREX BYFLOAT increments by the given amount} {
        r del foo
        assert_match {0.1* 0.1*} [r increx foo byfloat 0.1]
        assert_match {0.3* 0.2*} [r increx foo byfloat 0.2]
        assert_match {0.3*} [r get foo]
    }
    
    test {INCREX NX only sets when key does not exist} {
        r del foo
        assert_equal {1 1} [r increx foo nx]
        assert_equal {1 0} [r increx foo nx]
        assert_equal {1} [r get foo]
    }

    test {INCREX BYFLOAT NX only sets when key does not exist} {
        r del foo
        assert_match {*0.1* *0.1*} [r increx foo nx byfloat 0.1]
        assert_match {*0.1* 0} [r increx foo nx byfloat 0.1]
        assert_match {*0.1*} [r get foo]
    }

    test {INCREX XX only sets when key already exists} {
        r del foo
        assert_equal {0 0} [r increx foo xx]
        assert_equal {0} [r exists foo]
        r set foo 10
        assert_equal {11 1} [r increx foo xx]
    }

    test {INCREX BYFLOAT XX only sets when key already exist} {
        r del foo
        assert_match {0 0} [r increx foo xx byfloat 0.1]
        r set foo 0.1
        assert_match {*0.2* *0.1*} [r increx foo xx byfloat 0.1]
        assert_match {*0.2*} [r get foo]
    }

    test {INCREX NX and XX are mutually exclusive} {
        r del foo
        catch {r increx foo nx xx} err
        format $err
    } {ERR*}

    test {INCREX with EX sets a TTL} {
        r del foo
        r increx foo ex 100
        assert_range [r ttl foo] 1 100
    }

    test {INCREX with PX sets a TTL in milliseconds} {
        r del foo
        r increx foo px 100000
        assert_range [r pttl foo] 1 100000
    }

    test {INCREX with EXAT in the past deletes/skips the key} {
        r set foo 5
        r increx foo exat 1
        assert_equal {0} [r exists foo]
    }

    test {INCREX combines EX and BYINT correctly} {
        r del foo
        r increx foo ex 100 byint 7
        assert_equal {7} [r get foo]
        assert_range [r ttl foo] 1 100
    }

    test {INCREX combines EX and BYFLOAT correctly} {
        r del foo
        r increx foo ex 100 byfloat 2.5
        assert_equal {2.5} [r get foo]
        assert_range [r ttl foo] 1 100
    }

    test {INCREX combines NX, EX, and BYINT correctly} {
        r del foo
        r increx foo nx ex 100 byint 3
        assert_equal {3} [r get foo]
        assert_range [r ttl foo] 1 100
        # second call should no-op since key now exists
        assert_equal {3 0} [r increx foo nx ex 100 byint 3]
    }

    test {INCREX BYINT and BYFLOAT are mutually exclusive} {
        r del foo
        catch {r increx foo byint 1 byfloat 1.0} err
        format $err
    } {ERR*}

    test {INCREX overflow protection} {
        r set foo 9223372036854775807
        assert_equal [r increx foo byint 1] {9223372036854775807 0}
    }

    test {INCREX BYFLOAT does not allow Infinity} {
        r set foo 0
        catch {r increx foo byfloat +inf} err
        format $err
    } {ERR *BYFLOAT increment cannot be Infinity*} {valgrind:skip}

    test {INCREX BYFLOAT does not allow nan} {
        r set foo 0
        catch {r increx foo byfloat nan} err
        format $err
    } {ERR *Increment is not a valid float*} {valgrind:skip}

    test {INCREX BYFLOAT does not allow exponentials} {
        r set foo 0
        catch {r increx foo byfloat 1e99999} err
        format $err
    } {ERR *Increment is not a valid float*} {valgrind:skip}

    test {INCREX BYFLOAT does not allow inf values} {
        r set foo inf
        catch {r increx foo byfloat 1} err
        format $err
    } {ERR *value cannot be Infinity*} {valgrind:skip}

    test {INCREX BYINT does not allow inf values} {
        r set foo inf
        catch {r increx foo byint 1} err
        format $err
    } {ERR *value is not an integer or out of range*} {valgrind:skip}

    test {INCREX distinguishes a bad increment from a bad stored value} {
        # The two have opposite causes - the caller's argument is wrong, or the
        # data is - so they must not report the same thing.
        r set foo 10
        assert_error "ERR Increment is not an integer or out of range" {r increx foo byint abc}
        assert_error "ERR Increment is not a valid float" {r increx foo byfloat abc}
        r set foo abc
        assert_error "ERR value is not an integer or out of range" {r increx foo byint 1}
        assert_error "ERR value is not a valid float" {r increx foo byfloat 1}
    }

    test {INCREX BYFLOAT positive arithmetic overflow returns [curr_val, 0]} {
        set big [ldbl_overflow_operand]
        r del foo
        r set foo $big
        # big + big overflows to infinity: should not error, and should leave the
        # value alone while reporting a zero delta.
        set res [r increx foo byfloat $big]
        assert_equal 0 [lindex $res 1]
        assert_equal $big [r get foo]
    }
    test {INCREX BYFLOAT negative arithmetic overflow returns [curr_val, 0]} {
        set big [ldbl_overflow_operand]
        r del foo
        r set foo -$big
        # -big + -big overflows to -infinity
        set res [r increx foo byfloat -$big]
        assert_equal 0 [lindex $res 1]
        assert_equal -$big [r get foo]
    }
    test {INCREX BYFLOAT overflow preserves existing TTL} {
        set big [ldbl_overflow_operand]
        r del foo
        r set foo $big ex 100
        r increx foo byfloat $big
        assert_range [r ttl foo] 1 100
        assert_equal $big [r get foo]
    }
    test {INCREX BYFLOAT overflow does not apply the command's expiration} {
        set big [ldbl_overflow_operand]
        # A rejected operation should not set a TTL on a key that has none...
        r del foo
        r set foo $big
        r increx foo byfloat $big ex 60
        assert_equal -1 [r ttl foo]
        # ...nor overwrite one that already exists.
        r del foo
        r set foo $big ex 100
        r increx foo byfloat $big ex 60
        assert_range [r ttl foo] 61 100
    }

    test {INCREX reports the increment that was actually applied} {
        # A long double cannot represent every integer at these magnitudes, so
        # the addition rounds and the delta that lands can differ from the one
        # that was asked for. By how much depends on how wide a long double is,
        # which varies by platform, so assert the invariant rather than any
        # particular value: the reply describes what happened, which means
        # `new == old + applied` has to hold everywhere.
        foreach {seed incr} {
            100000000000000000000 1
            100000000000000000000 3
            100000000000000000000 7
            100000000000000000000 9
            100000000000000000000 -1
            1000000000000000000000000000000 1
            1000000000000000000000000000000 7
            1000000000000000000000000000000 100000000000
        } {
            r del foo
            r set foo $seed
            # SET stores the literal string, but INCREX round-trips the value
            # through a long double, so a magnitude this large can be rewritten
            # just by being read. Normalize first - and check on the way past
            # that a zero increment reports a zero delta.
            assert_equal 0 [lindex [r increx foo byfloat 0] 1]
            set old [r get foo]
            set res [r increx foo byfloat $incr]
            set new [lindex $res 0]
            set applied [lindex $res 1]
            assert_equal $new [r get foo]
            assert_equal $new [expr {$old + $applied}]
        }
    }

    test {INCREX reports the requested increment when nothing is rounded} {
        r del foo
        r set foo 10
        assert_equal {15 5} [r increx foo byint 5]
        assert_equal {12.5 -2.5} [r increx foo byfloat -2.5]
        r del foo
        assert_equal {5 5} [r increx foo byint 5]
    }

    test {INCREX against key holding a list} {
        r del mylist
        r rpush mylist 1
        catch {r increx mylist} err
        r del mylist
        format $err
    } {WRONGTYPE*}

    test {INCREX preserves existing TTL when expire option omitted} {
        r del foo
        r set foo 1 ex 100
        r increx foo byint 1
        assert_range [r ttl foo] 1 100
    }

    test {INCREX wrong number of arguments} {
        assert_error "*ERR*" {r increx}
    }

    test {INCREX against key holding a list, with already-expired EXAT} {
        r del list_key
        r rpush list_key a
        assert_error {WRONGTYPE*} {r increx list_key exat 1}
        r del list_key
    } 

    test {INCREX against non-numeric string value, with already-expired EXAT} {
        r set str abc
        assert_error {ERR*} {r increx str exat 1}
        r del str
    }

    test {INCREX against nonexistent key with already-expired EXAT does not store key} {
        r del non_existing
        assert_equal {1 1} [r increx non_existing exat 1]
        assert_equal 0 [r exists non_existing]
    }

    test {INCREX BYINT with missing value is a syntax error} {
        r del key
        assert_error {ERR*} {r increx key byint}
    }

    test {INCREX reply types on the wire - RESP3} {
        r hello 3
        r readraw 1
        r del foo
        assert_equal {*2} [r increx foo byint 5]
        assert_equal {:5} [r read]
        assert_equal {:5} [r read]
        # Default increment is integer mode.
        assert_equal {*2} [r increx foo]
        assert_equal {:6} [r read]
        assert_equal {:1} [r read]
        # Both elements are RESP3 doubles in float mode, not bulk strings.
        r del foo
        assert_equal {*2}   [r increx foo byfloat 2.5]
        assert_equal {,2.5} [r read]
        assert_equal {,2.5} [r read]
        # XX on nonexistent key returns [0, 0]
        r del foo
        assert_equal {*2} [r increx foo xx]
        assert_equal {:0} [r read]
        assert_equal {:0} [r read]
        # XX on nonexistent key with BYFLOAT returns [0.0, 0.0]
        assert_equal {*2} [r increx foo xx byfloat 1.5]
        assert_equal {,0} [r read]
        assert_equal {,0} [r read]
        r readraw 0
        r hello 2
    }
    
    test {INCREX reply types on the wire - RESP2} {
        if {!$::force_resp3} {
            r readraw 1
            r del foo
            assert_equal {*2} [r increx foo byint 5]
            assert_equal {:5} [r read]
            assert_equal {:5} [r read]
            # BYFLOAT degrades to bulk strings under RESP2.
            r del foo
            assert_equal {*2}  [r increx foo byfloat 2.5]
            assert_equal {$3}  [r read]
            assert_equal {2.5} [r read]
            assert_equal {$3}  [r read]
            assert_equal {2.5} [r read]
            # XX on nonexistent key returns [0, 0]
            r del foo
            assert_equal {*2}  [r increx foo xx]
            assert_equal {:0}  [r read]
            assert_equal {:0}  [r read]
            # XX on nonexistent key with BYFLOAT returns ["0", "0"]
            assert_equal {*2}  [r increx foo xx byfloat 1.5]
            assert_equal {$1}  [r read]
            assert_equal {0}   [r read]
            assert_equal {$1}  [r read]
            assert_equal {0}   [r read]
            r readraw 0
        }
    }

    test {INCREX LBOUND and UBOUND in integer mode} {
        r set k 10
        assert_equal {15 5} [r increx k byint 5 ubound 20]
        assert_equal 15 [r get k]

        r set k 10
        assert_equal {15 5} [r increx k byint 5 ubound 15]
        assert_equal 15 [r get k]

        # Exceeds UBOUND without SATURATE: declined
        r set k 10
        assert_equal {10 0} [r increx k byint 5 ubound 12]
        assert_equal 10 [r get k]

        r set k 10
        assert_equal {10 0} [r increx k byint 5 ubound 10]
        assert_equal 10 [r get k]

        r set k 10
        assert_equal {10 0} [r increx k byint 5 ubound 5]
        assert_equal 10 [r get k]

        # Bound constrains result, not direction
        r set k 10
        assert_equal {5 -5} [r increx k byint -5 ubound 5]
        assert_equal 5 [r get k]

        r set k 10
        assert_equal {5 -5} [r increx k byint -5 lbound 0]
        assert_equal 5 [r get k]

        r set k 10
        assert_equal {5 -5} [r increx k byint -5 lbound 5]
        assert_equal 5 [r get k]

        # Exceeds LBOUND without SATURATE: declined
        r set k 10
        assert_equal {10 0} [r increx k byint -5 lbound 8]
        assert_equal 10 [r get k]

        r set k 10
        assert_equal {10 0} [r increx k byint 5 lbound 20]
        assert_equal 10 [r get k]

        # Both bounds specified
        r set k 10
        assert_equal {15 5} [r increx k byint 5 lbound 0 ubound 20]
        assert_equal 15 [r get k]

        # LBOUND == UBOUND
        r set k 10
        assert_equal {10 0} [r increx k byint 5 lbound 10 ubound 10]
        assert_equal 10 [r get k]
    }

    test {INCREX SATURATE clamps to bound} {
        r set k 10
        assert_equal {12 2} [r increx k byint 5 ubound 12 saturate]
        assert_equal 12 [r get k]

        r set k 10
        assert_equal {10 0} [r increx k byint 5 ubound 10 saturate]
        assert_equal 10 [r get k]

        # Clamps to bound even when moving opposite to requested increment!
        r set k 10
        assert_equal {5 -5} [r increx k byint 5 ubound 5 saturate]
        assert_equal 5 [r get k]

        r set k 10
        assert_equal {8 -2} [r increx k byint -5 lbound 8 saturate]
        assert_equal 8 [r get k]

        r set k 10
        assert_equal {20 10} [r increx k byint -5 lbound 20 saturate]
        assert_equal 20 [r get k]

        r set k 10
        assert_equal {10 0} [r increx k byint 5 lbound 10 ubound 10 saturate]
        assert_equal 10 [r get k]

        # SATURATE with no bounds is plain increment
        r set k 10
        assert_equal {15 5} [r increx k byint 5 saturate]
        assert_equal 15 [r get k]
        assert_equal {16 1} [r increx k saturate]
        assert_equal 16 [r get k]

        # SATURATE on integer overflow/underflow clamps to type limits or bounds
        r set k 9223372036854775807
        assert_equal {9223372036854775807 0} [r increx k byint 1 saturate]
        assert_equal {9223372036854775807 0} [r increx k byint 1 ubound 9223372036854775807 saturate]

        r set k [expr {9223372036854775807 - 7}]
        assert_equal {9223372036854775807 7} [r increx k byint 10 saturate]
        assert_equal 9223372036854775807 [r get k]

        r set k 10
        assert_equal {100 90} [r increx k byint 9223372036854775807 ubound 100 saturate]
        assert_equal 100 [r get k]

        # Integer overflow with UBOUND but without SATURATE is declined
        r set k 10
        assert_equal {10 0} [r increx k byint 9223372036854775807 ubound 100]
        assert_equal 10 [r get k]

        r set k [expr {-9223372036854775808 + 7}]
        assert_equal {-9223372036854775808 -7} [r increx k byint -10 saturate]
        assert_equal -9223372036854775808 [r get k]

        r set k -10
        assert_equal {-100 -90} [r increx k byint -9223372036854775807 lbound -100 saturate]
        assert_equal -100 [r get k]

        r set k -10
        assert_equal {-10 0} [r increx k byint -9223372036854775807 lbound -100]
        assert_equal -10 [r get k]
    }

    test {INCREX LBOUND, UBOUND, SATURATE in float mode} {
        r set k 10
        assert_equal {12.5 2.5} [r increx k byfloat 2.5 lbound 5.0 ubound 15.0]
        assert_equal 12.5 [r get k]

        # Out of bounds without SATURATE: declined
        r set k 10
        assert_equal {10 0} [r increx k byfloat 5.5 ubound 12.5]
        assert_equal 10 [r get k]

        # SATURATE clamps in float mode
        r set k 10
        assert_equal {12.5 2.5} [r increx k byfloat 5.5 ubound 12.5 saturate]
        assert_equal 12.5 [r get k]

        r set k 10
        assert_equal {8 -2} [r increx k byfloat -6.0 lbound 8.0 saturate]
        assert_equal 8 [r get k]

        # Float overflow with UBOUND and SATURATE
        set big [ldbl_overflow_operand]
        r set k 10
        assert_equal {100 90} [r increx k byfloat $big ubound 100 saturate]
        assert_equal 100 [r get k]

        # Float overflow with UBOUND without SATURATE: declined
        r set k 10
        assert_equal {10 0} [r increx k byfloat $big ubound 100]
        assert_equal 10 [r get k]

        # Float underflow with LBOUND and SATURATE
        r set k 10
        assert_equal {-100 -110} [r increx k byfloat -$big lbound -100 saturate]
        assert_equal -100 [r get k]

        # Float underflow with LBOUND without SATURATE: declined
        r set k 10
        assert_equal {10 0} [r increx k byfloat -$big lbound -100]
        assert_equal 10 [r get k]

        # Float overflow with SATURATE and no bounds saturates to type limit
        r set k $big
        set res [r increx k byfloat $big saturate]
        assert_equal [r get k] [lindex $res 0]
    }

    test {INCREX bounds on missing key: creation depends on SATURATE} {
        r del k
        assert_equal {1 1} [r increx k ubound 5]
        assert_equal 1 [r exists k]
        assert_equal 1 [r get k]

        r del k
        assert_equal {0 0} [r increx k ubound 0]
        assert_equal 0 [r exists k]

        r del k
        assert_equal {0 0} [r increx k ubound 0 saturate]
        assert_equal 1 [r exists k]
        assert_equal 0 [r get k]

        r del k
        assert_equal {0 0} [r increx k lbound 5]
        assert_equal 0 [r exists k]

        r del k
        assert_equal {5 5} [r increx k lbound 5 saturate]
        assert_equal 1 [r exists k]
        assert_equal 5 [r get k]
    }

    test {INCREX bounds with expiration} {
        # Bound-declined does not apply TTL
        r set k 10
        assert_equal {10 0} [r increx k byint 5 ubound 5 ex 100]
        assert_equal -1 [r ttl k]

        r set k 10 ex 500
        assert_equal {10 0} [r increx k byint 5 ubound 5 ex 100]
        assert_range [r ttl k] 400 500

        # Saturated success applies TTL
        r set k 10
        assert_equal {12 2} [r increx k byint 5 ubound 12 saturate ex 100]
        assert_range [r ttl k] 1 100
    }

    test {INCREX bound validation errors} {
        r set k 10
        assert_error "*ERR LBOUND can't be greater than UBOUND*" {r increx k byint 5 lbound 20 ubound 0}
        assert_error "*ERR LBOUND can't be greater than UBOUND*" {r increx k byint 5 lbound 20 ubound 0 saturate}
        assert_error "*ERR LBOUND can't be greater than UBOUND*" {r increx k byfloat 1.0 lbound 20.0 ubound 0.0}

        assert_error "*ERR UBOUND is not an integer or out of range*" {r increx k byint 5 ubound 12.5}
        assert_error "*ERR UBOUND is not an integer or out of range*" {r increx k ubound abc}
        assert_error "*ERR LBOUND is not an integer or out of range*" {r increx k lbound abc}

        assert_error "*ERR UBOUND is not a valid float*" {r increx k byfloat 1.0 ubound abc}
        assert_error "*ERR LBOUND is not a valid float*" {r increx k byfloat 1.0 lbound abc}

        # Fractional bound is valid in float mode
        assert_equal {11 1} [r increx k byfloat 1.0 lbound 0.5 ubound 15.5]

        # Missing bound value is a syntax error
        assert_error "*ERR syntax error*" {r increx k lbound}
        assert_error "*ERR syntax error*" {r increx k ubound}

        # Inf bounds validation
        assert_error "*ERR LBOUND can't be greater than UBOUND*" {r increx k byfloat 1.0 ubound -inf}
        assert_error "*ERR LBOUND can't be greater than UBOUND*" {r increx k byfloat 1.0 lbound inf}
        assert_error "*ERR LBOUND can't be greater than UBOUND*" {r increx k byfloat 1.0 lbound +inf}
        assert_error "*ERR LBOUND can't be greater than UBOUND*" {r increx k byfloat 1.0 lbound inf ubound -inf}
        r set k 10
        assert_equal {11 1} [r increx k byfloat 1.0 ubound inf]
        r set k 10
        assert_equal {11 1} [r increx k byfloat 1.0 ubound +inf]
        r set k 10
        assert_equal {11 1} [r increx k byfloat 1.0 lbound -inf]
        r set k 10
        assert_equal {10 0} [r increx k byfloat 1.0 lbound -inf ubound -inf]
        r set k 10
        assert_equal {10 0} [r increx k byfloat 1.0 lbound inf ubound inf]

        # Integer mode rejects inf for bounds
        assert_error "*ERR UBOUND is not an integer or out of range*" {r increx k ubound inf}
        assert_error "*ERR LBOUND is not an integer or out of range*" {r increx k lbound inf}
    }

    test {INCREX applied delta overflow errors} {
        r set k 9223372036854775807
        assert_error "*ERR applied increment would overflow*" {r increx k byint -5 ubound -9223372036854775808 saturate}

        set big [ldbl_overflow_operand]
        r set k -$big
        assert_error "*ERR applied increment would be Infinity*" {r increx k byfloat $big lbound $big saturate}

        r set k 10
        assert_error "*ERR applied increment would be Infinity*" {r increx k byfloat 1.0 lbound -inf ubound -inf saturate}
        assert_error "*ERR applied increment would be Infinity*" {r increx k byfloat 1.0 lbound inf ubound inf saturate}
    }

    test {INCREX PERSIST option} {
        # Removes TTL on existing key with TTL
        r set k 10 ex 500
        assert_equal {11 1} [r increx k persist]
        assert_equal -1 [r ttl k]
        assert_equal 11 [r get k]

        # Key without TTL remains persisted
        r set k 10
        assert_equal {11 1} [r increx k persist]
        assert_equal -1 [r ttl k]

        # Absent key created with no TTL
        r del k
        assert_equal {1 1} [r increx k persist]
        assert_equal -1 [r ttl k]
        assert_equal 1 [r get k]

        # Overflow declined preserves existing TTL
        r set k 9223372036854775807 ex 500
        assert_equal {9223372036854775807 0} [r increx k byint 1 persist]
        assert_range [r ttl k] 400 500

        # Bound declined preserves existing TTL
        r set k 10 ex 500
        assert_equal {10 0} [r increx k byint 5 ubound 5 persist]
        assert_range [r ttl k] 400 500

        # PERSIST is mutually exclusive with expiration options and ENX
        assert_error "*ERR syntax error*" {r increx k ex 10 persist}
        assert_error "*ERR syntax error*" {r increx k persist ex 10}
        assert_error "*ERR syntax error*" {r increx k persist enx}
        assert_error "*ERR syntax error*" {r increx k enx persist}
    }

    test {INCREX ENX option} {
        r set k 10
        assert_error "*ERR ENX flag requires an expiration*" {r increx k enx}

        # Key without TTL: expiration is applied
        r set k 10
        assert_equal {11 1} [r increx k ex 100 enx]
        assert_range [r ttl k] 1 100

        # Key WITH TTL: increment applied, existing TTL preserved
        r set k 10 ex 500
        assert_equal {11 1} [r increx k ex 100 enx]
        assert_range [r ttl k] 400 500

        # Key WITH TTL: PX expiration preserved
        r set k 10 ex 500
        assert_equal {11 1} [r increx k px 100000 enx]
        assert_range [r ttl k] 400 500

        # Absent key: expiration is applied
        r del k
        assert_equal {1 1} [r increx k ex 100 enx]
        assert_range [r ttl k] 1 100

        # Past EXAT on key without TTL deletes key
        r set k 10
        assert_equal {11 1} [r increx k exat 100 enx]
        assert_equal 0 [r exists k]

        # Past EXAT on key WITH TTL keeps key and preserves TTL
        r set k 10 ex 500
        assert_equal {11 1} [r increx k exat 100 enx]
        assert_equal 1 [r exists k]
        assert_range [r ttl k] 400 500

        # Overflow declined does not apply expiration
        r set k 9223372036854775807
        assert_equal {9223372036854775807 0} [r increx k byint 1 ex 100 enx]
        assert_equal -1 [r ttl k]
    }

    test {INCRBY INCRBYFLOAT DECRBY against unhappy path} {
        r del mykeyincr
        assert_error "*ERR wrong number of arguments*" {r incr mykeyincr v}
        assert_error "*ERR wrong number of arguments*" {r decr mykeyincr v}
        assert_error "*value is not an integer or out of range*" {r incrby mykeyincr v}
        assert_error "*value is not an integer or out of range*" {r incrby mykeyincr 1.5}
        assert_error "*value is not an integer or out of range*" {r decrby mykeyincr v}
        assert_error "*value is not an integer or out of range*" {r decrby mykeyincr 1.5}
        assert_error "*value is not a valid float*" {r incrbyfloat mykeyincr v}
    }

    foreach cmd {"incr" "decr" "incrby" "decrby" "increx"} {
        test "$cmd operation should update encoding from raw to int" {
            set res {}
            set expected {1 12}
            if {[string match {*incr*} $cmd]} {
                lappend expected 13
            } else {
                lappend expected 11
            }

            r set foo 1
            assert_encoding "int" foo
            lappend res [r get foo]

            r append foo 2
            assert_encoding "raw" foo
            lappend res [r get foo]

            if {[string match {*by*} $cmd]} {
                r $cmd foo 1
            } else {
                r $cmd foo
            }
            assert_encoding "int" foo
            lappend res [r get foo]
            assert_equal $res $expected
        }
    }
}
