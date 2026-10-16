set testmodule [file normalize tests/modules/keyspace_unsubscribe.so]

tags "modules" {
    start_server [list overrides [list loadmodule "$testmodule"]] {

        test {Unsubscribe removes only the given event types} {
            r set s 1
            r hset h f 1
            assert_equal {1 1 0 0} [r ksu.counts]

            assert_equal OK [r ksu.unsub string]
            r set s 2
            r hset h f 2
            assert_equal {1 2 0 0} [r ksu.counts]

            assert_equal OK [r ksu.unsub hash]
            r set s 3
            r hset h f 3
            assert_equal {1 2 0 0} [r ksu.counts]
        }

        test {Unsubscribe of types not subscribed fails} {
            assert_error {*not subscribed*} {r ksu.unsub string}
            assert_error {*not subscribed*} {r ksu.unsub hash}
        }

        test {Subscribe again after unsubscribe} {
            assert_equal OK [r ksu.sub]
            r set s 4
            r hset h f 4
            assert_equal {2 3 0 0} [r ksu.counts]
        }

        test {Callback can unsubscribe itself} {
            r lpush l a
            r lpush l b
            r rpop l
            assert_equal {2 3 1 0} [r ksu.counts]
        }

        test {Callback can unsubscribe the next subscriber in the list} {
            r sadd set a
            r sadd set b
            assert_equal {2 3 1 0} [r ksu.counts]
        }

        test {Unsubscribe within MULTI} {
            r multi
            r set s 5
            r ksu.unsub string
            r set s 6
            r exec
            assert_equal {3 3 1 0} [r ksu.counts]
        }

        test {Unload module after unsubscribe} {
            assert_equal {OK} [r module unload keyspace_unsubscribe]
            r set s 7
        } {OK}
    }
}
