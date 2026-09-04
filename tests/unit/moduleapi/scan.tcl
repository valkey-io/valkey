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
        assert_encoding btree zz1
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

    # ---- VM_ScanKeyRawBorrowed: borrowed (ptr,len) scan, must match native scan ----

    test {Module scan_key_raw hash listpack} {
        r del rh
        r hmset rh f1 v1 f2 v2
        assert_encoding listpack rh
        lsort [r scan.scan_key_raw rh]
    } {{f1 v1} {f2 v2}}

    test {Module scan_key_raw hash listpack with int value} {
        r del rh1
        r hmset rh1 f1 1
        assert_encoding listpack rh1
        lsort [r scan.scan_key_raw rh1]
    } {{f1 1}}

    test {Module scan_key_raw hash dict} {
        r del rh3
        r hmset rh3 f1 v1 f2 v2 f3 v3
        assert_encoding hashtable rh3
        lsort [r scan.scan_key_raw rh3]
    } {{f1 v1} {f2 v2} {f3 v3}}

    test {Module scan_key_raw zset listpack} {
        r del rz
        r zadd rz 1 f1 2 f2
        assert_encoding listpack rz
        lsort [r scan.scan_key_raw rz]
    } {{f1 1} {f2 2}}

    test {Module scan_key_raw zset btree} {
        r del rz1
        r zadd rz1 1 f1 2 f2 3 f3
        assert_encoding btree rz1
        lsort [r scan.scan_key_raw rz1]
    } {{f1 1} {f2 2} {f3 3}}

    test {Module scan_key_raw zset fractional score (d2string form)} {
        r del rz2
        r zadd rz2 1.5 f1 2 f2 3 f3
        assert_encoding btree rz2
        assert_equal [lsort [r zrange rz2 0 -1 withscores]] [lsort {f1 1.5 f2 2 f3 3}]
        lsort [r scan.scan_key_raw rz2]
    } {{f1 1.5} {f2 2} {f3 3}}

    test {Module scan_key_raw set intset} {
        r del rs
        r sadd rs 1 2
        assert_encoding intset rs
        lsort [r scan.scan_key_raw rs]
    } {{1 {}} {2 {}}}

    test {Module scan_key_raw set dict} {
        r del rsa
        r sadd rsa 1 2 ; # Created as intset
        r sadd rsa 3   ; # Converted to hashtable
        assert_encoding hashtable rsa
        lsort [r scan.scan_key_raw rsa]
    } {{1 {}} {2 {}} {3 {}}}

    test {Module scan_key_raw set listpack} {
        r del rs1
        r sadd rs1 a b c
        assert_encoding listpack rs1
        lsort [r scan.scan_key_raw rs1]
    } {{a {}} {b {}} {c {}}}

    test "Unload the module - scan" {
        assert_equal {OK} [r module unload scan]
    }
}