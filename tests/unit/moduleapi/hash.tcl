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

    test {Module HSETEX propagates one expiry with aliased argument objects} {
        set repl [attach_to_replication_stream]
        set expected [list [list select *]]
        foreach {option value} [list EX 100 PX 100000 EXAT [expr [clock seconds]+100]] {
            assert_equal 1 [r hash.hsetex $option $option $value]
            assert_equal v [r hget $option f]
            set expiry [lindex [r hpexpiretime $option FIELDS 1 f] 0]
            lappend expected [list hsetex $option PXAT $expiry FIELDS 1 f v]
        }
        assert_replication_stream $repl $expected
        close_replication_stream $repl
    } {} {needs:repl}

    test "Unload the module - hash" {
        assert_equal {OK} [r module unload hash]
    }
}
