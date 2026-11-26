set testmodule [file normalize tests/modules/zset.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module zset rem} {
        r del k
        r zadd k 100 hello 200 world
        assert_equal 1 [r zset.rem k hello]
        assert_equal 0 [r zset.rem k hello]
        assert_equal 1 [r exists k]
        # Check that removing the last element deletes the key
        assert_equal 1 [r zset.rem k world]
        assert_equal 0 [r exists k]
    }

    test {Module zset add} {
        r del k
        # Check that failure does not create empty key
        assert_error "ERR ZsetAdd failed" {r zset.add k nan hello}
        assert_equal 0 [r exists k]

        r zset.add k 100 hello
        assert_equal {hello 100} [r zrange k 0 -1 withscores]
    }

    test {Module zset incrby} {
        r del k
        # Check that failure does not create empty key
        assert_error "ERR ZsetIncrby failed" {r zset.incrby k hello nan}
        assert_equal 0 [r exists k]

        r zset.incrby k hello 100
        assert_equal {hello 100} [r zrange k 0 -1 withscores]
    }

    test {Module zset rangebylex} {
        # Should give wrong arity error
        assert_error "ERR wrong number of arguments*" {r zset.rangebylex}
        assert_error "ERR wrong number of arguments*" {r zset.revrangebylex}

        # Should give wrong type error
        r del k
        r set k v
        assert_error "WRONGTYPE Operation against a key*" {r zset.rangebylex k - +}

        # Should give invalid range error
        r del k
        r zadd k 0 ele
        assert_error "invalid range" {r zset.rangebylex k - a}
        assert_error "invalid range" {r zset.revrangebylex k - a}

        # Check if the data structure of the sorted set is skiplist
        r del k
        r config set zset-max-listpack-entries 2
        r config set zset-max-listpack-value 64
        for {set i 0} {$i < 4} {incr i} {
            r zadd k 0 "ele$i"
        }
        assert_equal {ele0 ele1 ele2 ele3} [r zset.rangebylex k - +]
        assert_equal {ele3 ele2 ele1 ele0} [r zset.revrangebylex k - +]
        assert_equal {ele1 ele2} [r zset.rangebylex k "(ele0" "(ele3"]
        assert_equal {ele2 ele1} [r zset.revrangebylex k "(ele0" "(ele3"]

        # Check if the data structure of the sorted set is listpack
        r del k
        r config set zset-max-listpack-entries 128
        r config set zset-max-listpack-value 64
        for {set i 0} {$i < 4} {incr i} {
            r zadd k 0 "ele$i"
        }
        assert_equal {ele0 ele1 ele2 ele3} [r zset.rangebylex k - +]
        assert_equal {ele3 ele2 ele1 ele0} [r zset.revrangebylex k - +]
        assert_equal {ele1 ele2} [r zset.rangebylex k "(ele0" "(ele3"]
        assert_equal {ele2 ele1} [r zset.revrangebylex k "(ele0" "(ele3"]
    }

    test {Module zset members} {
        # Should give wrong arity error
        assert_error "ERR wrong number of arguments*" {r zset.members}

        # Should give wrong type error
        r del k
        r set k v
        assert_error "WRONGTYPE Operation against a key*" {r zset.members k}

        # Check if the data structure of the sorted set is skiplist
        r del k
        r config set zset-max-listpack-entries 2
        r config set zset-max-listpack-value 64
        for {set i 0} {$i < 4} {incr i} {
            r zadd k 0 "ele$i"
        }
        assert_equal {ele0 ele1 ele2 ele3} [lsort [r zset.members k]]

        # Check if the data structure of the sorted set is listpack
        r del k
        r config set zset-max-listpack-entries 128
        r config set zset-max-listpack-value 64
        for {set i 0} {$i < 4} {incr i} {
            r zadd k 0 "ele$i"
        }
        assert_equal {ele0 ele1 ele2 ele3} [lsort [r zset.members k]]
    }

    foreach {enc max_entries} {listpack 128 btree 0} {
        r config set zset-max-listpack-entries $max_entries

        test "Module zset lex range with an empty or inverted range starts with no element - $enc" {
            r del lex
            r zadd lex 0 a 0 b 0 c 0 d 0 e
            assert_equal $enc [r object encoding lex]
            assert_equal {{} END} [r zset.walk lex lex \[c \[b n]
            assert_equal {{} END} [r zset.walk lex lex ( ( n]
            assert_equal {{} END} [r zset.walk lex lex-last (e + p]
            assert_equal {{} END} [r zset.walk lex lex-last \[f + p]
            assert_equal {} [r zset.rangebylex lex \[c \[b]
            assert_equal {} [r zset.revrangebylex lex (e +]
            # A non-empty range still starts on its first element.
            assert_equal {b c} [r zset.walk lex lex \[b \[d n]
            assert_equal {d c} [r zset.walk lex lex-last \[b \[d p]
            assert_error {ERR no such key} {r zset.walk nokey lex - + n}
        }

        test "Module zset range iteration steps to the neighbour in both directions - $enc" {
            r del k lex
            r zadd k 1 a 2 b 3 c 4 d 5 e
            r zadd lex 0 a 0 b 0 c 0 d 0 e
            assert_equal $enc [r object encoding k]
            # Walk the range to its end and back to its start.
            assert_equal {b c d END c b END} [r zset.walk k score 2 4 nnnppp]
            assert_equal {b c d END c b END} [r zset.walk lex lex \[b \[d nnnppp]
            # Reversing direction must not revisit the current element.
            assert_equal {b c b c b} [r zset.walk k score 2 4 npnp]
            assert_equal {b c b c b} [r zset.walk lex lex \[b \[d npnp]
            # Stepping back from the first element ends the iteration and
            # leaves the cursor usable.
            assert_equal {b END c d} [r zset.walk k score 2 4 pnn]
            assert_equal {b END c d} [r zset.walk lex lex \[b \[d pnn]
            # Walking backwards from the last element of the range.
            assert_equal {d c b END} [r zset.walk k score-last 2 4 ppp]
            assert_equal {d c b END} [r zset.walk lex lex-last \[b \[d ppp]
            assert_equal {d END c} [r zset.walk k score-last 2 4 np]
            # Same, with the range reaching both ends of the key.
            assert_equal {a b c d e END d c b a END b} [r zset.walk k score -inf +inf nnnnnpppppn]
            assert_equal {a b c d e END d c b a END b} [r zset.walk lex lex - + nnnnnpppppn]
        }

        test "Module zset range end-reached flag follows the last step - $enc" {
            r del k
            r zadd k 1 a 2 b 3 c 4 d 5 e
            assert_equal $enc [r object encoding k]
            assert_equal {b 0 c d END 1 c 0 b END 1 c 0} [r zset.walk k score 2 4 ennnepeppene]
            assert_equal {{} 1 END 1} [r zset.walk k score 10 20 ene]
        }

        test "Module zset score range with a NaN bound is empty - $enc" {
            r del k
            r zadd k 1 a 2 b
            assert_equal $enc [r object encoding k]
            assert_equal {{} END} [r zset.walk k score 0 nan n]
            assert_equal {{} END} [r zset.walk k score nan 5 n]
            assert_equal {{} END} [r zset.walk k score-last 0 nan p]
            assert_equal {{} END} [r zset.walk k score-last nan 5 p]
            assert_equal {a b} [r zset.walk k score 0 5 n]
        }

        test "Module zset range iteration with delete through key handle - $enc" {
            r del k
            r zadd k 1 a 2 b 3 c 4 d
            assert_equal $enc [r object encoding k]
            assert_equal {b c} [r zset.iter_rem k 2 3]
            assert_equal {a d} [r zrange k 0 -1]
            assert_equal {a d} [r zset.iter_rem k -inf +inf]
            assert_equal 0 [r exists k]

            if {$enc eq "btree"} {
                set expected {}
                for {set i 0} {$i < 200} {incr i} {
                    set m [format "member_%03d_padding_for_leaf_nodes" $i]
                    r zadd k $i $m
                    if {$i >= 150} { lappend expected $m }
                }
                assert_equal btree [r object encoding k]
                assert_equal $expected [r zset.iter_rem k 150 199]
                assert_equal 150 [r zcard k]
            }
        }

        test "Module zset range iteration with delete through ValkeyModule_Call - $enc" {
            r del k
            r zadd k 1 a 2 b 3 c 4 d
            assert_equal $enc [r object encoding k]
            assert_equal {d 0 END 1} [r zset.iter_call_rem k 0 -2]

            r del k
            r zadd k 1 a 2 b 3 c 4 d
            assert_equal $enc [r object encoding k]
            assert_equal {{} 0 b 0} [r zset.iter_call_rem k 2 3]
        }

        test "Module zset range iteration with score increase through key handle - $enc" {
            # Raising the member just returned moves it ahead of the walk's
            # position, so it is returned again, and the walk goes on until the
            # command's own 100-step guard: a is returned until it passes b,
            # then a and b leapfrog until they pass c, then all three cycle.
            r del k
            r zadd k 1 a 5 b 9 c
            assert_equal $enc [r object encoding k]
            set expected [concat [lrepeat 5 a] [lrepeat 4 b a] b c [lrepeat 28 a b c] a]
            assert_equal $expected [r zset.iter_incr k 1 -inf +inf fwd]
            assert_equal {b 38 c 38 a 39} [r zrange k 0 -1 withscores]

            # Two members raised in turn keep leapfrogging each other.
            r del k
            r zadd k 1 a 1.5 b
            assert_equal [lrepeat 50 a b] [r zset.iter_incr k 1 -inf +inf fwd]

            # Lowering scores on a forward walk moves them behind the position:
            # each member once.
            r del k
            r zadd k 1 a 5 b 9 c
            assert_equal {a b c} [r zset.iter_incr k -1 -inf +inf fwd]
            assert_equal {a 0 b 4 c 8} [r zrange k 0 -1 withscores]

            # Raising scores on a reverse walk moves them behind: each once.
            r del k
            r zadd k 1 a 1.5 b 9 c
            assert_equal {c b a} [r zset.iter_incr k 1 -inf +inf rev]
            assert_equal {a 2 b 2.5 c 10} [r zrange k 0 -1 withscores]

            # A member moved out of the range is not returned again; the
            # members still in range are.
            r del k
            r zadd k 1 a 5 b 9 c 20 d
            assert_equal {a b c} [r zset.iter_incr k 100 -inf 10 fwd]
            assert_equal {d 20 a 101 b 105 c 109} [r zrange k 0 -1 withscores]
        }

        test "Module zset range iteration with score changes ahead of and behind the position - $enc" {
            r del k
            r zadd k 1 a 2 b 3 c
            assert_equal $enc [r object encoding k]
            # The member just returned, moved ahead through the key handle, is
            # returned again when the walk reaches its new position.
            assert_equal {a b a c} [r zset.iter_mut k score -inf +inf fwd 0 add a 2.5]
            # A member moved behind the position is not returned.
            r del k
            r zadd k 1 a 2 b 3 c
            assert_equal {a b} [r zset.iter_mut k score -inf +inf fwd 1 add c 0]
            # The same through ValkeyModule_Call.
            r del k
            r zadd k 1 a 2 b 3 c
            assert_equal {a b a c} [r zset.iter_mut k score -inf +inf fwd 0 call ZINCRBY k 1.5 a]
            assert_equal {b 2 a 2.5 c 3} [r zrange k 0 -1 withscores]
        }

        test "Module zset range iteration stepped from a notification callback inside a Call - $enc" {
            r del k src gone
            r zadd k 1 a 2 b 3 c
            r zadd src 10 x 20 y
            assert_equal $enc [r object encoding k]
            # ZUNIONSTORE deletes the expired source key first and fires
            # "expired"; the callback steps the walk, attaching it to k's
            # current value. Then it deletes k ("del") and stores the union
            # ("zunionstore"), with no module hook between these writes. The
            # walk is detached again when the callback's context is freed, so
            # every member is still returned once, in order: the "del" step
            # finds no key, the "zunionstore" step continues in the new set.
            r debug set-active-expire 0
            test_defer {r debug set-active-expire 1}
            r zadd gone 0 z
            r pexpire gone 1
            # Wait on the server clock, not on key gone: reading it would
            # expire it before the module command does.
            lassign [r time] secs usecs
            set deadline [expr {$secs * 1000 + $usecs / 1000 + 2}]
            wait_for_condition 100 10 {
                [lassign [r time] s u; expr {$s * 1000 + $u / 1000}] > $deadline
            } else {
                fail "server time did not pass the expiry deadline"
            }
            assert_equal {a {b c} {x y}} [r zset.iter_notify k ZUNIONSTORE k 3 k src gone]
        }

        test "Module zset range walk of a thread safe context ends when its lock is released - $enc" {
            r del k
            r zadd k 1 a 2 b 3 c
            assert_equal $enc [r object encoding k]
            # The thread unlocks, then frees its context without closing the
            # key: the walk must already be gone from the server's list.
            assert_equal {b} [r zset.thread_walk k 0]
            assert_equal PONG [r ping]
            # Locking again does not revive the walk: the step reports the end.
            assert_equal {b 0 1} [r zset.thread_walk k 1]
            assert_equal PONG [r ping]
        }

        test "Module zset lex range walk agrees with ZRANGEBYLEX on a mixed-score set - $enc" {
            r del k
            r zadd k 0 b 0 d 1 a 1 c 2 e
            assert_equal $enc [r object encoding k]
            # Only the far bound in the direction of travel is checked, as the
            # core commands do: on mixed scores the near bound fails for
            # members that follow in (score, member) order.
            set fwd [r zrangebylex k {[c} +]
            set rev [r zrevrangebylex k + {[c}]
            assert_equal $fwd [r zset.walk_range k lex {[c} + fwd 0]
            assert_equal $rev [r zset.walk_range k lex {[c} + rev 0]
            # The same with a Call before every step, so each step re-seeks.
            assert_equal $fwd [r zset.walk_range k lex {[c} + fwd 1]
            assert_equal $rev [r zset.walk_range k lex {[c} + rev 1]
        }

        test "Module zset range walk of a thread safe context survives ValkeyModule_Yield - $enc" {
            r del k
            r zadd k 1 a 2 b 3 c
            assert_equal $enc [r object encoding k]
            test_config_set r busy-reply-threshold 1
            # The thread yields for 100 ms: the main thread runs in between,
            # and its own releases of the lock must not end the walk either.
            set rd [valkey_deferring_client]
            $rd zset.thread_yield_walk k 100
            assert_equal {a b c} [$rd read]
            $rd close
            assert_equal PONG [r ping]
        }

        test "Module zset range walk continues on a key that expired logically while open - $enc" {
            r del k
            r zadd k 1 a 2 b 3 c
            assert_equal $enc [r object encoding k]
            r debug set-active-expire 0
            test_defer {r debug set-active-expire 1}
            test_config_set r busy-reply-threshold 1
            r pexpire k 20
            # The thread yields for 100 ms; the main thread runs in between, so
            # the server clock passes the TTL while the walk is open. The walk
            # re-finds the key without an expiry check, as the handle never
            # did: the step moves and the add lands on the existing key instead
            # of creating it again.
            set rd [valkey_deferring_client]
            $rd zset.thread_expired_add k 100 d 4
            assert_equal {1 4} [$rd read]
            $rd close
            assert_equal PONG [r ping]
            r del k
        }

        test "Module zset range iteration current element after a change - $enc" {
            r del k
            r zadd k 1 a 2 b
            assert_equal $enc [r object encoding k]
            # The entry at the position is (1, a). Once a's score changed it is
            # no longer in the set, so there is no current element.
            assert_equal NULL [r zset.cur_after_incr k a 1]
            r del k
            r zadd k 1 a 2 b
            assert_equal {a 1} [r zset.cur_after_incr k b 1]
        }

        test "Module zset range iteration reverse step leaving the range after a change - $enc" {
            r del k
            r zadd k 1 a 2 b 3 c 4 d
            assert_equal $enc [r object encoding k]
            assert_equal {c} [r zset.iter_mut k score 2 3 rev 0 call ZREM k b]
            r del k
            r zadd k 0 a 0 b 0 c 0 d
            assert_equal {c} [r zset.iter_mut k lex {[b} {[c} rev 0 call ZREM k b]
        }

        test "Module zset lex range iteration with changes mid-walk - $enc" {
            # Remove the current member through the key handle.
            r del k
            r zadd k 0 a 0 b 0 c 0 d 0 e
            assert_equal $enc [r object encoding k]
            assert_equal {b c d} [r zset.iter_mut k lex {[b} {[d} fwd 0 rem b]
            assert_equal {a c d e} [r zrange k 0 -1]
            r del k
            r zadd k 0 a 0 b 0 c 0 d 0 e
            assert_equal {d c b} [r zset.iter_mut k lex {[b} {[d} rev 0 rem d]
            assert_equal {a b c e} [r zrange k 0 -1]

            # Add a member ahead of the walk: visited. Behind it: not visited.
            r del k
            r zadd k 0 a 0 b 0 c 0 d 0 e
            assert_equal {b bb c d} [r zset.iter_mut k lex {[b} {[d} fwd 0 add bb 0]
            r del k
            r zadd k 0 a 0 b 0 c 0 d 0 e
            assert_equal {b c d} [r zset.iter_mut k lex {[b} {[d} fwd 1 add ba 0]
            assert_equal {a b ba c d e} [r zrange k 0 -1]

            # Changes through ValkeyModule_Call, forward and reverse.
            r del k
            r zadd k 0 a 0 b 0 c 0 d 0 e
            assert_equal {a b bb c d e} [r zset.iter_mut k lex - + fwd 1 call ZADD k 0 bb]
            r del k
            r zadd k 0 a 0 b 0 c 0 d 0 e
            assert_equal {d b} [r zset.iter_mut k lex {[b} {[d} rev 0 call ZREM k c]
        }
    }
    r config set zset-max-listpack-entries 128

    foreach {enc max_entries} {listpack 128 btree 0} {
        test "Module key handle closes cleanly after a Call turns its string into a $enc zset" {
            r config set zset-max-listpack-entries $max_entries
            r del k src
            # A RAW string makes dbSetValue swap the zset into the robj the
            # handle still points at. An EMBSTR would be replaced instead.
            r set k [string repeat x 300]
            assert_equal raw [r object encoding k]
            r zadd src 1 a 2 b 3 c
            assert_equal $enc [r object encoding src]
            # Key type 1 is STRING, 5 is ZSET.
            assert_equal {1 5} [r zset.close_after_call k src]
            assert_equal {a b c} [r zrange k 0 -1]
            assert_equal PONG [r ping]
        }

        test "Module zset range walk through a handle whose string a Call turned into a $enc zset" {
            test_config_set r zset-max-listpack-entries $max_entries
            r del k src
            r set k [string repeat x 300]
            assert_equal raw [r object encoding k]
            r zadd src 1 a 2 b 3 c
            assert_equal $enc [r object encoding src]
            assert_equal {a b c} [r zset.walk_after_call k src]
            assert_equal PONG [r ping]
        }
    }
    r config set zset-max-listpack-entries 128

    test {Module zset range iteration across listpack to btree conversion} {
        test_config_set r zset-max-listpack-value 64
        r del k
        r zadd k 1 a 3 c
        assert_equal listpack [r object encoding k]
        set big [string repeat b 70]
        assert_equal [list a $big c] [r zset.iter_add k 2 $big]
        assert_equal btree [r object encoding k]
    }

    test "Unload the module - zset" {
        assert_equal {OK} [r module unload zset]
    }
}

# The module stays loaded in this server: the test leaks key handles on
# purpose and keeps them reachable, so the leak checker must still see the
# module's data when the server exits.
start_server {tags {"modules"}} {
    r module load $testmodule

    foreach {type max_lp_entries} {listpack 128 btree 0} {
        r config set zset-max-listpack-entries $max_lp_entries
        test "Module zset range walk on a key handle the module never closed - $type" {
            r del k
            r zadd k 1 a 2 b
            assert_equal $type [r object encoding k]
            assert_equal OK [r zset.walk_leak k]
            # The walk's handle was never closed. The server stopped the walk
            # when the command's context was freed, so changing the set and
            # then running a module command that detaches walks is safe.
            r del k
            r zadd k 5 x
            assert_equal {x} [r zset.iter_mut k score -inf +inf fwd 0 call PING]
            assert_equal PONG [r ping]
        }
        test "Module zset range walk left attached by a callback without a module context - $type" {
            r config set zset-max-listpack-entries $max_lp_entries
            r del k k2
            r zadd k 1 a 2 b 3 c
            r zadd k2 1 a
            assert_equal $type [r object encoding k]
            assert_equal OK [r zset.oneshot_walk k]
            wait_for_condition 100 10 {[r zset.oneshot_status] == 1} else {
                fail "one-shot callback did not run"
            }
            # The walk's handle stays open with no command around it. Grow the
            # set so the listpack moves, then run any module hook.
            for {set i 0} {$i < 20} {incr i} {
                r zadd k $i [string repeat m$i 10]
            }
            assert_equal {a} [r zset.iter_mut k2 score -inf +inf fwd 0 call PING]
            assert_equal OK [r zset.oneshot_cleanup]
            assert_equal PONG [r ping]
        }

    }
}
