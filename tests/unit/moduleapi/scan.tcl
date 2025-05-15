set testmodule [file normalize tests/modules/scan.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module scan keyspace} {
        # the module create a scan command with filtering which also return values
        r set x 1
        r set y 2
        r set z 3
        r hset h f v
        lsort [r scan.scan_strings]
    } {{x 1} {y 2} {z 3}}

    test {Module scan hash listpack} {
        r hmset hh f1 v1 f2 v2
        assert_encoding listpack hh
        lsort [r scan.scan_key hh]
    } {{f1 v1} {f2 v2}}

    test {Module scan hash listpack with int value} {
        r hmset hh1 f1 1
        assert_encoding listpack hh1
        lsort [r scan.scan_key hh1]
    } {{f1 1}}

    test {Module scan hash dict} {
        r config set hash-max-ziplist-entries 2
        r hmset hh3 f1 v1 f2 v2 f3 v3
        assert_encoding hashtable hh3
        lsort [r scan.scan_key hh3]
    } {{f1 v1} {f2 v2} {f3 v3}}

    test {Module scan zset listpack} {
        r zadd zz 1 f1 2 f2
        assert_encoding listpack zz
        lsort [r scan.scan_key zz]
    } {{f1 1} {f2 2}}

    test {Module scan zset skiplist} {
        r config set zset-max-ziplist-entries 2
        r zadd zz1 1 f1 2 f2 3 f3
        assert_encoding skiplist zz1
        lsort [r scan.scan_key zz1]
    } {{f1 1} {f2 2} {f3 3}}

    test {Module scan set intset} {
        r del ss
        r sadd ss 1 2
        assert_encoding intset ss
        lsort [r scan.scan_key ss]
    } {{1 {}} {2 {}}}

    test {Module scan set dict} {
        r del ssa
        r config set set-max-intset-entries 2
        r sadd ssa 1 2 ; # Created as intset
        r sadd ssa 3   ; # Converted to hashtable
        assert_encoding hashtable ssa
        lsort [r scan.scan_key ssa]
    } {{1 {}} {2 {}} {3 {}}}

    test {Module scan set listpack} {
        r del ss1
        r sadd ss1 a b c
        assert_encoding listpack ss1
        lsort [r scan.scan_key ss1]
    } {{a {}} {b {}} {c {}}}

    test "Unload the module - scan" {
        assert_equal {OK} [r module unload scan]
    }
}