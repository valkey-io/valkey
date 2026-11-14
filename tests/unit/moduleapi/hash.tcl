set testmodule [file normalize tests/modules/hash.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module hash set} {
        r set k mystring
        assert_error "WRONGTYPE*" {r hash.set k "" hello world}
        r del k
        # "" = count updates and deletes of existing fields only
        assert_equal 0 [r hash.set k "" squirrel yes]
        # "a" = COUNT_ALL = count inserted, modified and deleted fields
        assert_equal 2 [r hash.set k "a" banana no sushi whynot]
        # "n" = NX = only add fields not already existing in the hash
        # "x" = XX = only replace the value for existing fields
        assert_equal 0 [r hash.set k "n" squirrel hoho what nothing]
        assert_equal 1 [r hash.set k "na" squirrel hoho something nice]
        assert_equal 0 [r hash.set k "xa" new stuff not inserted]
        assert_equal 1 [r hash.set k "x" squirrel ofcourse]
        assert_equal 1 [r hash.set k "" sushi :delete: none :delete:]
        r hgetall k
    } {squirrel ofcourse banana no what nothing something nice}

    set original_max [lindex [r config get hash-max-listpack-entries] 1]
    foreach {type max_entries} {listpack 128 hashtable 0} {
        test "Module hash set KEEPTTL retains field expiration - $type" {
            r config set hash-max-listpack-entries $max_entries
            r del k
            r hset k f1 v1 f2 v2
            assert_encoding $type k
            r hpexpire k 100000 FIELDS 2 f1 f2

            # Without KEEPTTL, overwriting a field clears its expiration.
            assert_equal 1 [r hash.set k "" f1 new1]
            assert_equal {-1} [r httl k FIELDS 1 f1]

            # With KEEPTTL, the existing expiration is retained.
            set expiretime [r hpexpiretime k FIELDS 1 f2]
            assert_equal 1 [r hash.set k "k" f2 new2]
            assert_equal $expiretime [r hpexpiretime k FIELDS 1 f2]

            # KEEPTTL on a new field does not add an expiration.
            assert_equal 1 [r hash.set k "ka" f3 v3]
            assert_equal {-1} [r httl k FIELDS 1 f3]

            # KEEPTTL on a field without expiration keeps it persistent.
            assert_equal 1 [r hash.set k "k" f1 newer1]
            assert_equal {-1} [r httl k FIELDS 1 f1]

            assert_equal {newer1 new2 v3} [r hmget k f1 f2 f3]
            assert_encoding $type k
        }

        test "Module hash set KEEPTTL does not revive an expired field - $type" {
            r config set hash-max-listpack-entries $max_entries
            r debug set-active-expire 0
            r del k
            r hset k f1 v1 f2 v2
            assert_encoding $type k
            r hpexpire k 1 FIELDS 1 f1
            after 10

            # The expired field is treated as new: it is not counted as an
            # update and does not inherit the stale expiration.
            set repl [attach_to_replication_stream]
            assert_equal 0 [r hash.set k "k" f1 new1]
            assert_equal {-1} [r httl k FIELDS 1 f1]
            assert_equal {new1} [r hget k f1]

            # Replicas never see the field as expired, so it must be deleted
            # there before the command is replayed.
            assert_replication_stream $repl {
                {multi}
                {select *}
                {hdel k f1}
                {hash.set k k f1 new1}
                {exec}
            }
            close_replication_stream $repl
            r debug set-active-expire 1
        } {OK} {needs:debug needs:repl}
    }
    r config set hash-max-listpack-entries $original_max

    test "Module hash set KEEPTTL over an expired field keeps the replica in sync" {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            set primary [srv -1 client]
            set replica [srv 0 client]
            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_sync $replica

            $primary debug set-active-expire 0
            $primary del k
            $primary hset k f1 v1
            $primary hpexpire k 1 FIELDS 1 f1
            after 10
            $primary hash.set k "k" f1 new1
            wait_for_ofs_sync $primary $replica

            assert_equal {new1} [$replica hget k f1]
            assert_equal {-1} [$replica httl k FIELDS 1 f1]
            $primary debug set-active-expire 1
        }
    } {} {needs:debug needs:repl}

    test "Unload the module - hash" {
        assert_equal {OK} [r module unload hash]
    }
}
