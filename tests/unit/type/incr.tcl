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

    test {No negative zero} {
        r del foo
        r increx foo byfloat [expr double(1)/41]
        r increx foo byfloat [expr double(-1)/41]
        r get foo
    } {0}

    test {INCREX default increment is 1} {
        r del foo
        r increx foo
    } {1}

    test {INCREX BYINT increments by given amount} {
        r del foo
        r increx foo byint 5
        r increx foo byint 5
    } {10}

    test {INCREX NX only sets when key does not exist} {
        r del foo
        assert_equal {1} [r increx foo nx]
        assert_equal {} [r increx foo nx]
        assert_equal {1} [r get foo]
    }

    test {INCREX XX only sets when key already exists} {
        r del foo
        assert_equal {} [r increx foo xx]
        assert_equal {0} [r exists foo]
        r set foo 10
        assert_equal {11} [r increx foo xx]
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
        assert_equal {} [r increx foo nx ex 100 byint 3]
    }

    test {INCREX BYINT and BYFLOAT are mutually exclusive} {
        r del foo
        catch {r increx foo byint 1 byfloat 1.0} err
        format $err
    } {ERR*}

    test {INCREX overflow protection} {
        r set foo 9223372036854775807
        catch {r increx foo byint 1} err
        format $err
    } {ERR*overflow*}

    test {INCREX BYFLOAT does not allow NaN or Infinity} {
        r set foo 0
        catch {r increx foo byfloat +inf} err
        format $err
    } {ERR *would produce*} {valgrind:skip}

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
