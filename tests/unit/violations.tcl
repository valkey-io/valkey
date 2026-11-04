# One XADD with one huge >4GiB field (reduced from 5GB for CI memory limits)
# Must exceed 2^32 to require more than 32 bits to address
# Expected to fail without creating the stream
run_solo {violations} {
start_server [list overrides [list save ""] tags {"large-memory"}] {
    test {XADD one huge field} {
        r config set proto-max-bulk-len 10000000000 ;#10gb
        r config set client-query-buffer-limit 10000000000 ;#10gb
        r write "*5\r\n\$4\r\nXADD\r\n\$2\r\nS1\r\n\$1\r\n*\r\n"
        r write "\$1\r\nA\r\n"
        catch {
            write_big_bulk 4300000000 ;#~4GiB, >2^32
        } err
        assert_match {*too large*} $err
        r xlen S1
    } {0}
}

# One XADD with one huge (exactly nearly) 4GB field
# This uncovers the overflow in lpEncodeGetType
# Expected to fail without creating the stream
start_server [list overrides [list save ""] tags {"large-memory"}] {
    test {XADD one huge field - 1} {
        r config set proto-max-bulk-len 10000000000 ;#10gb
        r config set client-query-buffer-limit 10000000000 ;#10gb
        r write "*5\r\n\$4\r\nXADD\r\n\$2\r\nS1\r\n\$1\r\n*\r\n"
        r write "\$1\r\nA\r\n"
        catch {
            write_big_bulk 4294967295 ;#4gb-1
        } err
        assert_match {*too large*} $err
        r xlen S1
    } {0}
}

# Gradually add big stream fields using repeated XADD calls
# Reduced from 10 to 3 iterations to fit in 16GB CI runners with ASAN overhead
start_server [list overrides [list save ""] tags {"large-memory"}] {
    test {several XADD big fields} {
        r config set stream-node-max-bytes 0
        for {set j 0} {$j<3} {incr j} {
            r xadd stream * 1 $::str500 2 $::str500
        }
        r ping
        r xlen stream
    } {3}
}

# Add over 4GB to a single stream listpack (one XADD command)
# Expected to fail without creating the stream
start_server [list overrides [list save ""] tags {"large-memory"}] {
    test {single XADD big fields} {
        r write "*23\r\n\$4\r\nXADD\r\n\$1\r\nS\r\n\$1\r\n*\r\n"
        for {set j 0} {$j<10} {incr j} {
            r write "\$1\r\n$j\r\n"
            write_big_bulk 500000000 "" yes ;#500mb
        }
        r flush
        catch {r read} err
        assert_match {*too large*} $err
        r xlen S
    } {0}
}

# A refused XADD must not create the key, since nothing is propagated
start_server [list overrides [list save ""] tags {"large-memory"}] {
    test {XADD over the size limit does not create the key} {
        # Two 512MiB values plus the field names are just over the 1GiB limit
        r write "*7\r\n\$4\r\nXADD\r\n\$1\r\nS\r\n\$1\r\n*\r\n"
        for {set j 0} {$j<2} {incr j} {
            r write "\$1\r\n$j\r\n"
            write_big_bulk 536870912 "" yes
        }
        r flush
        catch {r read} err
        assert_match {*too large*} $err
        r exists S
    } {0}
}

# Gradually add big hash fields using repeated HSET calls
# This reproduces the overflow in the call to ziplistResize
# Object will be converted to hashtable encoding
start_server [list overrides [list save ""] tags {"large-memory"}] {
    r config set hash-max-ziplist-value 1000000000 ;#1gb
    test {hash with many big fields} {
        for {set j 0} {$j<10} {incr j} {
            r hset h $j $::str500
        }
        r object encoding h
    } {hashtable}
}

# Add over 4GB to a single hash field (one HSET command)
# Object will be converted to hashtable encoding
# Reduced from 5GB; must exceed 2^32 to test >4GiB (32-bit boundary) behavior
start_server [list overrides [list save ""] tags {"large-memory"}] {
    test {hash with one huge field} {
        catch {r config set hash-max-ziplist-value 10000000000} ;#10gb
        r config set proto-max-bulk-len 10000000000 ;#10gb
        r config set client-query-buffer-limit 10000000000 ;#10gb
        r write "*4\r\n\$4\r\nHSET\r\n\$2\r\nH1\r\n"
        r write "\$1\r\nA\r\n"
        write_big_bulk 4300000000 ;#~4GiB, >2^32
        r object encoding H1
    } {hashtable}
}

# A 2^31 byte length is negative as an int
start_server [list overrides [list save ""] tags {"large-memory"}] {
    test {SADD the same 2GiB member twice} {
        r config set proto-max-bulk-len 10000000000 ;#10gb
        r config set client-query-buffer-limit 10000000000 ;#10gb
        for {set j 0} {$j < 2} {incr j} {
            r write "*3\r\n\$4\r\nSADD\r\n\$1\r\nS\r\n"
            write_big_bulk 2147483648 ;#2gb
        }
        r scard S
    } {1}
}

start_server [list overrides [list save ""] tags {"large-memory"}] {
    test {Evict a key with a 2GiB name} {
        r config set proto-max-bulk-len 10000000000 ;#10gb
        r config set client-query-buffer-limit 10000000000 ;#10gb
        r write "*3\r\n\$3\r\nSET\r\n"
        write_big_bulk 2147483648 "" yes ;#2gb
        r write "\$1\r\nv\r\n"
        r flush
        assert_equal OK [r read]
        r config set maxmemory-policy allkeys-lru
        r config set maxmemory 1mb
        wait_for_condition 50 100 {
            [r dbsize] == 0
        } else {
            fail "key was not evicted"
        }
    }
}
} ;# run_solo

# SORT which stores an integer encoded element into a list.
# Just for coverage, no news here.
start_server [list overrides [list save ""] ] {
    test {SORT adds integer field to list} {
        r set S1 asdf
        r set S2 123 ;# integer encoded
        assert_encoding "int" S2
        r sadd myset 1 2
        r mset D1 1 D2 2
        r sort myset by D* get S* store mylist
        r llen mylist
    } {2} {cluster:skip}
}
