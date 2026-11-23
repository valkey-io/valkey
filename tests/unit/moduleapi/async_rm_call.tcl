set testmodule [file normalize tests/modules/blockedclient.so]
set testmodule2 [file normalize tests/modules/postnotifications.so]
set testmodule3 [file normalize tests/modules/blockonkeys.so]

foreach cmd {rm_call vm_call_argv } {

    start_server {tags {"modules"}} {
        r module load $testmodule

        test "Locked GIL acquisition from async call - $cmd" {
            assert_equal {OK} [r do_${cmd}_async acquire_gil]
        }

        test "Blpop on async call fire and forget - $cmd" {
            assert_equal {Blocked} [r do_${cmd}_fire_and_forget blpop l 0]
            r lpush l a
            assert_equal {0} [r llen l]
        }

        test "Blpop on threaded async call - $cmd" {
            set rd [valkey_deferring_client]

            $rd do_${cmd}_async_on_thread blpop l 0
            wait_for_blocked_clients_count 1
            r lpush l a
            assert_equal [$rd read] {l a}
            wait_for_blocked_clients_count 0
            $rd close
        }

        foreach ncmd [list "do_${cmd}_async" "do_${cmd}_async_script_mode"] {

            test "Blpop on async call using $cmd" {
                set rd [valkey_deferring_client]

                $rd $ncmd blpop l 0
                wait_for_blocked_clients_count 1
                r lpush l a
                assert_equal [$rd read] {l a}
                wait_for_blocked_clients_count 0
                $rd close
            }

            test "Brpop on async call using $cmd" {
                set rd [valkey_deferring_client]

                $rd $ncmd brpop l 0
                wait_for_blocked_clients_count 1
                r lpush l a
                assert_equal [$rd read] {l a}
                wait_for_blocked_clients_count 0
                $rd close
            }

            test "Brpoplpush on async call using $cmd" {
                set rd [valkey_deferring_client]

                $rd $ncmd brpoplpush l1 l2 0
                wait_for_blocked_clients_count 1
                r lpush l1 a
                assert_equal [$rd read] {a}
                wait_for_blocked_clients_count 0
                $rd close
                r lpop l2
            } {a}

            test "Blmove on async call using $cmd" {
                set rd [valkey_deferring_client]

                $rd $ncmd blmove l1 l2 LEFT LEFT 0
                wait_for_blocked_clients_count 1
                r lpush l1 a
                assert_equal [$rd read] {a}
                wait_for_blocked_clients_count 0
                $rd close
                r lpop l2
            } {a}

            test "Bzpopmin on async call using $cmd" {
                set rd [valkey_deferring_client]

                $rd $ncmd bzpopmin s 0
                wait_for_blocked_clients_count 1
                r zadd s 10 foo
                assert_equal [$rd read] {s foo 10}
                wait_for_blocked_clients_count 0
                $rd close
            }

            test "Bzpopmax on async call using $cmd" {
                set rd [valkey_deferring_client]

                $rd $ncmd bzpopmax s 0
                wait_for_blocked_clients_count 1
                r zadd s 10 foo
                assert_equal [$rd read] {s foo 10}
                wait_for_blocked_clients_count 0
                $rd close
            }
        }

        test "Nested async $cmd" {
            set rd [valkey_deferring_client]

            $rd do_${cmd}_async do_${cmd}_async do_${cmd}_async do_${cmd}_async blpop l 0
            wait_for_blocked_clients_count 1
            r lpush l a
            assert_equal [$rd read] {l a}
            wait_for_blocked_clients_count 0
            $rd close
        }

        test "Test multiple async call $cmd waiting on the same event" {
            set rd1 [valkey_deferring_client]
            set rd2 [valkey_deferring_client]

            $rd1 do_${cmd}_async do_${cmd}_async do_${cmd}_async do_${cmd}_async blpop l 0
            $rd2 do_${cmd}_async do_${cmd}_async do_${cmd}_async do_${cmd}_async blpop l 0
            wait_for_blocked_clients_count 2
            r lpush l element element
            assert_equal [$rd1 read] {l element}
            assert_equal [$rd2 read] {l element}
            wait_for_blocked_clients_count 0
            $rd1 close
            $rd2 close
        }

        test "async call $cmd calls RM_Call" {
            assert_equal {PONG} [r do_${cmd}_async do_rm_call ping]
        }

        test "async call $cmd calls background RM_Call calls RM_Call" {
            assert_equal {PONG} [r do_${cmd}_async do_bg_rm_call do_rm_call ping]
        }

        test "async call $cmd calls background RM_Call calls RM_Call calls async call" {
            assert_equal {PONG} [r do_${cmd}_async do_bg_rm_call do_rm_call do_${cmd}_async ping]
        }

        test "async call - $cmd - inside async call callback" {
            set rd [valkey_deferring_client]
            $rd wait_and_do_${cmd} blpop l 0
            wait_for_blocked_clients_count 1

            start_server {} {
                test "Connect a replica to the master instance" {
                    r slaveof [srv -1 host] [srv -1 port]
                    wait_for_condition 50 100 {
                        [s role] eq {slave} &&
                        [string match {*master_link_status:up*} [r info replication]]
                    } else {
                        fail "Can't turn the instance into a replica"
                    }
                }

                assert_equal {1} [r -1 lpush l a]
                assert_equal [$rd read] {l a}
            }

            wait_for_blocked_clients_count 0
            $rd close
        }

        test "Become replica while having async call - $cmd - running" {
            r flushall
            set rd [valkey_deferring_client]
            $rd do_${cmd}_async blpop l 0
            wait_for_blocked_clients_count 1

            #become a replica of a not existing server
            r replicaof localhost 30000

            catch {[$rd read]} e
            assert_match {UNBLOCKED force unblock from blocking operation*} $e
            wait_for_blocked_clients_count 0

            r replicaof no one

            r lpush l 1
            # make sure the async rm_call was aborted
            assert_equal [r llen l] {1}
            $rd close
        }

        test "Pipeline with blocking RM_Call - $cmd" {
            r flushall
            set rd [valkey_deferring_client]
            set buf ""
            append buf "do_${cmd}_async blpop l 0\r\n"
            append buf "ping\r\n"
            $rd write $buf
            $rd flush
            wait_for_blocked_clients_count 1

            # release the blocked client
            r lpush l 1

            assert_equal [$rd read] {l 1}
            assert_equal [$rd read] {PONG}

            wait_for_blocked_clients_count 0
            $rd close
        }

        test "blocking RM_Call abort - $cmd" {
            r flushall
            set rd [valkey_deferring_client]

            $rd client id
            set client_id [$rd read]

            $rd do_${cmd}_async blpop l 0
            wait_for_blocked_clients_count 1

            r client kill ID $client_id
            assert_error {*error reading reply*} {$rd read}

            wait_for_blocked_clients_count 0

            r lpush l 1
            # make sure the async rm_call was aborted
            assert_equal [r llen l] {1}
            $rd close
        }
    }

    start_server {tags {"modules"}} {
        r module load $testmodule

        test "Test basic replication stream on unblock handler - $cmd" {
            r flushall
            set repl [attach_to_replication_stream]

            set rd [valkey_deferring_client]

            $rd do_${cmd}_async blpop l 0
            wait_for_blocked_clients_count 1
            r lpush l a
            assert_equal [$rd read] {l a}

            assert_replication_stream $repl {
                {select *}
                {lpush l a}
                {lpop l}
            }
            close_replication_stream $repl

            wait_for_blocked_clients_count 0
            $rd close
        }

        test "Test no propagation of blocking command - $cmd" {
            r flushall
            set repl [attach_to_replication_stream]

            set rd [valkey_deferring_client]

            $rd do_${cmd}_async_no_replicate blpop l 0
            wait_for_blocked_clients_count 1
            r lpush l a
            assert_equal [$rd read] {l a}

            # make sure the lpop are not replicated
            r set x 1

            assert_replication_stream $repl {
                {select *}
                {lpush l a}
                {set x 1}
            }
            close_replication_stream $repl

            wait_for_blocked_clients_count 0
            $rd close
        }

        test "Test unblock handler are executed as a unit - $cmd" {
            r flushall
            set repl [attach_to_replication_stream]

            set rd [valkey_deferring_client]

            $rd blpop_and_set_multiple_keys_with_${cmd} l x 1 y 2
            wait_for_blocked_clients_count 1
            r lpush l a
            assert_equal [$rd read] {OK}

            assert_replication_stream $repl {
                {select *}
                {lpush l a}
                {multi}
                {lpop l}
                {set x 1}
                {set y 2}
                {exec}
            }
            close_replication_stream $repl

            wait_for_blocked_clients_count 0
            $rd close
        }
    }

    start_server {tags {"modules"}} {
        r module load $testmodule
        r module load $testmodule2

        test "Test unblock handler are executed as a unit with key space notifications - $cmd" {
            r flushall
            set repl [attach_to_replication_stream]

            set rd [valkey_deferring_client]

            $rd blpop_and_set_multiple_keys_with_${cmd} l string_foo 1 string_bar 2
            wait_for_blocked_clients_count 1
            r lpush l a
            assert_equal [$rd read] {OK}

            # Explanation of the first multi exec block:
            # {lpop l} - pop the value by our blocking command 'blpop_and_set_multiple_keys_with_${cmd}'
            # {set string_foo 1} - the action of our blocking command 'blpop_and_set_multiple_keys_with_${cmd}'
            # {set string_bar 2} - the action of our blocking command 'blpop_and_set_multiple_keys_with_${cmd}'
            # {incr string_changed{string_foo}} - post notification job that was registered when 'string_foo' changed
            # {incr string_changed{string_bar}} - post notification job that was registered when 'string_bar' changed
            # {incr string_total} - post notification job that was registered when string_changed{string_foo} changed
            # {incr string_total} - post notification job that was registered when string_changed{string_bar} changed
            assert_replication_stream $repl {
                {select *}
                {lpush l a}
                {multi}
                {lpop l}
                {set string_foo 1}
                {set string_bar 2}
                {incr string_changed{string_foo}}
                {incr string_changed{string_bar}}
                {incr string_total}
                {incr string_total}
                {exec}
            }
            close_replication_stream $repl

            wait_for_blocked_clients_count 0
            $rd close
        }

        test "Test unblock handler are executed as a unit with lazy expire - $cmd" {
            r flushall
            r DEBUG SET-ACTIVE-EXPIRE 0
            set repl [attach_to_replication_stream]

            set rd [valkey_deferring_client]

            $rd blpop_and_set_multiple_keys_with_${cmd} l string_foo 1 string_bar 2
            wait_for_blocked_clients_count 1
            r lpush l a
            assert_equal [$rd read] {OK}

            # set expiration on string_foo
            r pexpire string_foo 1
            after 10

            # now the key should have been expired
            $rd blpop_and_set_multiple_keys_with_${cmd} l string_foo 1 string_bar 2
            wait_for_blocked_clients_count 1
            r lpush l a
            assert_equal [$rd read] {OK}

            # Explanation of the first multi exec block:
            # {lpop l} - pop the value by our blocking command 'blpop_and_set_multiple_keys_with_${cmd}'
            # {set string_foo 1} - the action of our blocking command 'blpop_and_set_multiple_keys_with_${cmd}'
            # {set string_bar 2} - the action of our blocking command 'blpop_and_set_multiple_keys_with_${cmd}'
            # {incr string_changed{string_foo}} - post notification job that was registered when 'string_foo' changed
            # {incr string_changed{string_bar}} - post notification job that was registered when 'string_bar' changed
            # {incr string_total} - post notification job that was registered when string_changed{string_foo} changed
            # {incr string_total} - post notification job that was registered when string_changed{string_bar} changed
            #
            # Explanation of the second multi exec block:
            # {lpop l} - pop the value by our blocking command 'blpop_and_set_multiple_keys_with_${cmd}'
            # {unlink string_foo} - lazy expiration of string_foo when 'blpop_and_set_multiple_keys_with_${cmd}' tries to write to it.
            # {set string_foo 1} - the action of our blocking command 'blpop_and_set_multiple_keys_with_${cmd}'
            # {set string_bar 2} - the action of our blocking command 'blpop_and_set_multiple_keys_with_${cmd}'
            # {incr expired} - the post notification job, registered after string_foo got expired
            # {incr string_changed{string_foo}} - post notification job triggered when we set string_foo
            # {incr string_changed{string_bar}} - post notification job triggered when we set string_bar
            # {incr string_total} - post notification job triggered when we incr 'string_changed{string_foo}'
            # {incr string_total} - post notification job triggered when we incr 'string_changed{string_bar}'
            assert_replication_stream $repl {
                {select *}
                {lpush l a}
                {multi}
                {lpop l}
                {set string_foo 1}
                {set string_bar 2}
                {incr string_changed{string_foo}}
                {incr string_changed{string_bar}}
                {incr string_total}
                {incr string_total}
                {exec}
                {pexpireat string_foo *}
                {lpush l a}
                {multi}
                {lpop l}
                {unlink string_foo}
                {set string_foo 1}
                {set string_bar 2}
                {incr expired}
                {incr string_changed{string_foo}}
                {incr string_changed{string_bar}}
                {incr string_total}
                {incr string_total}
                {exec}
            }
            close_replication_stream $repl
            r DEBUG SET-ACTIVE-EXPIRE 1

            wait_for_blocked_clients_count 0
            $rd close
        }
    }

    start_server {tags {"modules"}} {
        r module load $testmodule
        r module load $testmodule3

        test "Test unblock handler on module blocked on keys - $cmd" {
            set rd [valkey_deferring_client]

            r fsl.push l 1
            $rd do_${cmd}_async FSL.BPOPGT l 3 0
            wait_for_blocked_clients_count 1
            r fsl.push l 2
            r fsl.push l 3
            r fsl.push l 4
            assert_equal [$rd read] {4}

            wait_for_blocked_clients_count 0
            $rd close
            r fsl.clear l
        }
    }
}

start_server {tags {"modules"}} {
    r module load $testmodule

    test "RM_Call K command postponed by client pause is not re-executed after unpause" {
        r del l
        set rd [valkey_deferring_client]
        test_defer { $rd close }
        $rd do_rm_call_async blpop l 0
        wait_for_blocked_clients_count 1
        r client pause 60000 write
        # do_bg_rm_call runs RM_Call from a module thread, so the pause does not
        # hold it back. Its rpush wakes the BLPOP, whose re-run is postponed.
        r do_bg_rm_call rpush l x
        assert_equal 1 [r llen l]
        r client unpause
        # The user client is unblocked with an empty reply and must not pop x.
        wait_for_blocked_clients_count 0
        assert_equal 1 [r llen l]
        $rd ping
        assert_equal PONG [$rd read]
        r del l
    }
}

start_cluster 2 1 [list tags {modules external:skip cluster} overrides [list loadmodule $testmodule]] {
    # do_rm_call_async adds the '!' flag, so SETSLOT through a K call is
    # replicated once. Without '!' it would not be replicated (blockedclient.c).
    test "CLUSTER SETSLOT through an RM_Call that cannot block is applied on the primary (control)" {
        set target_id [R 1 CLUSTER MYID]
        # SETSLOT only waits for online replicas.
        wait_replica_online [Rn 0]

        # Inside MULTI the module drops the K flag, so SETSLOT cannot block: it
        # is applied at once and replicated afterwards.
        R 0 MULTI
        R 0 do_rm_call_async cluster setslot 1 migrating $target_id
        assert_equal OK [R 0 EXEC]
        assert_equal "\[1->-$target_id\]" [get_open_slots 0]
        wait_for_slot_state 2 "\[1->-$target_id\]"

        R 0 CLUSTER SETSLOT 1 STABLE
        wait_for_slot_state 2 ""
    }

    test "CLUSTER SETSLOT through an async RM_Call is applied on the primary and replies" {
        set target_id [R 1 CLUSTER MYID]
        wait_replica_online [Rn 0]

        # With the K flag the primary replicates SETSLOT first, blocks the call
        # until its replica acks, and only then applies it and replies.
        set rd [valkey_deferring_client]
        $rd do_rm_call_async cluster setslot 0 migrating $target_id
        wait_for_slot_state 2 "\[0->-$target_id\]"
        wait_for_blocked_clients_count 0

        wait_for_slot_state 0 "\[0->-$target_id\]"
        assert_equal OK [$rd read]

        $rd close
        R 0 CLUSTER SETSLOT 0 STABLE
        wait_for_slot_state 0 ""
        wait_for_slot_state 2 ""
    }

    test "CLUSTER SETSLOT through an async RM_Call is counted and replicated like a plain SETSLOT" {
        set target_id [R 1 CLUSTER MYID]
        wait_replica_online [Rn 0]

        proc setslot_calls {idx} {
            regexp {cmdstat_cluster\|setslot:calls=(\d+)} [R $idx info commandstats] -> calls
            return $calls
        }
        proc repl_offset {idx} {
            regexp {master_repl_offset:(\d+)} [R $idx info replication] -> off
            return $off
        }

        # Plain and K-call SETSLOT use slot numbers of the same length, so the
        # propagated argument sizes match.
        set calls0 [setslot_calls 0]
        set off0 [repl_offset 0]
        R 0 CLUSTER SETSLOT 5 MIGRATING $target_id
        set plain_calls [expr {[setslot_calls 0] - $calls0}]
        set plain_off [expr {[repl_offset 0] - $off0}]
        R 0 CLUSTER SETSLOT 5 STABLE
        wait_for_slot_state 2 ""

        set calls0 [setslot_calls 0]
        set off0 [repl_offset 0]
        assert_equal OK [R 0 do_rm_call_async cluster setslot 4 migrating $target_id]
        set k_calls [expr {[setslot_calls 0] - $calls0}]
        set k_off [expr {[repl_offset 0] - $off0}]
        R 0 CLUSTER SETSLOT 4 STABLE
        wait_for_slot_state 2 ""

        # Compare against a plain SETSLOT rather than a fixed count: a re-run
        # SETSLOT is counted twice until #5530, for every client.
        assert_equal $plain_calls $k_calls
        assert_equal $plain_off $k_off
    }
}
