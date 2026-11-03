set testmodule [file normalize tests/modules/keyspace_unsubscribe.so]
set keyspace_events [file normalize tests/modules/keyspace_events.so]

tags "modules" {
    start_server [list overrides [list loadmodule "$testmodule"]] {

        test {Unsubscribe removes only the given event types and callback} {
            r set s 1
            r hset h f 1
            assert_equal 1 [r ksu.count string]
            assert_equal 1 [r ksu.count hash]
            assert_equal 1 [r ksu.count other]

            assert_equal OK [r ksu.unsub string]
            r set s 2
            r hset h f 2
            assert_equal 1 [r ksu.count string]
            assert_equal 2 [r ksu.count hash]
            # Another callback subscribed to the same type is not affected.
            assert_equal 2 [r ksu.count other]

            assert_equal OK [r ksu.unsub hash]
            r set s 3
            r hset h f 3
            assert_equal 1 [r ksu.count string]
            assert_equal 2 [r ksu.count hash]
            assert_equal 3 [r ksu.count other]
        }

        test {Unsubscribe of types not subscribed fails} {
            assert_error {*not subscribed*} {r ksu.unsub string}
            assert_error {*not subscribed*} {r ksu.unsub hash}
        }

        test {Subscribe again after unsubscribe} {
            assert_equal OK [r ksu.sub]
            r set s 4
            r hset h f 4
            assert_equal 2 [r ksu.count string]
            assert_equal 3 [r ksu.count hash]
        }

        test {Callback can unsubscribe itself} {
            r lpush l a
            r lpush l b
            r rpop l
            assert_equal 1 [r ksu.count self]
        }

        test {Callback can unsubscribe the next subscriber in the list} {
            r sadd set a
            r sadd set b
            assert_equal 0 [r ksu.count victim]
        }

        test {Unsubscribe from within a nested dispatch} {
            r zadd z 1 a
            r zadd z 2 b
            assert_equal 0 [r ksu.count nested_victim]
            assert_equal 2 [r xlen nested_stream]
        }

        test {Callback can unsubscribe and subscribe itself again} {
            assert_equal OK [r ksu.notify k]
            assert_equal 1 [r ksu.count resubscribe]
            assert_equal OK [r ksu.notify k]
            assert_equal 2 [r ksu.count resubscribe]
        }

        test {Unsubscribe within MULTI} {
            r multi
            r set s 5
            r ksu.unsub string
            r set s 6
            assert_equal {OK OK OK} [r exec]
            assert_equal 3 [r ksu.count string]
        }

        test {Unload module after unsubscribe} {
            assert_equal {OK} [r module unload keyspace_unsubscribe]
            r set s 7
        } {OK}
    }

    start_server [list overrides [list loadmodule "$testmodule"]] {

        test {Unsubscribe does not affect subscriptions of other modules} {
            r module load $keyspace_events
            assert_equal OK [r ksu.unsub string]
            # keyspace_events counts STRING events on "string1_" keys in
            # "string_changed{<key>}".
            r set string1_x 1
            assert_equal 1 [r get string_changed{string1_x}]
        }
    }
}
