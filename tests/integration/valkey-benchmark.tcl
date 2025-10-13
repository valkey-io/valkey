source tests/support/benchmark.tcl
source tests/support/cli.tcl

proc cmdstat {cmd} {
    return [cmdrstat $cmd r]
}

# common code to reset stats, flush the db and run valkey-benchmark
proc common_bench_setup {cmd} {
    r config resetstat
    r flushall
    if {[catch { exec {*}$cmd } error]} {
        set first_line [lindex [split $error "\n"] 0]
        puts [colorstr red "valkey-benchmark non zero code, the output is: $error"]
        fail "valkey-benchmark non zero code. first line: $first_line"
    }
}

# we use this extra asserts on a simple set,get test for features like uri parsing
# and other simple flag related tests
proc default_set_get_checks {} {
    assert_match  {*calls=10,*} [cmdstat set]
    assert_match  {*calls=10,*} [cmdstat get]
    # assert one of the non benchmarked commands is not present
    assert_match  {} [cmdstat lrange]
}

tags {"benchmark network external:skip logreqres:skip"} {
    start_server {} {
        set master_host [srv 0 host]
        set master_port [srv 0 port]
        r select 0

        test {benchmark: set,get} {
            set cmd [valkeybenchmark $master_host $master_port "-c 5 -n 10 -t set,get"]
            common_bench_setup $cmd
            default_set_get_checks
        }

        test {benchmark: connecting using URI set,get} {
            set cmd [valkeybenchmarkuri $master_host $master_port "-c 5 -n 10 -t set,get"]
            common_bench_setup $cmd
            default_set_get_checks
        }

        test {benchmark: connecting using URI with authentication set,get} {
            r config set primaryauth pass
            set cmd [valkeybenchmarkuriuserpass $master_host $master_port "default" pass "-c 5 -n 10 -t set,get"]
            common_bench_setup $cmd
            default_set_get_checks
        }

        test {benchmark: full test suite} {
            set cmd [valkeybenchmark $master_host $master_port "-c 10 -n 100"]
            common_bench_setup $cmd

            # ping total calls are 2*issued commands per test due to PING_INLINE and PING_MBULK
            assert_match  {*calls=200,*} [cmdstat ping]
            assert_match  {*calls=100,*} [cmdstat set]
            assert_match  {*calls=100,*} [cmdstat get]
            assert_match  {*calls=100,*} [cmdstat incr]
            # lpush total calls are 2*issued commands per test due to the lrange tests
            assert_match  {*calls=200,*} [cmdstat lpush]
            assert_match  {*calls=100,*} [cmdstat rpush]
            assert_match  {*calls=100,*} [cmdstat lpop]
            assert_match  {*calls=100,*} [cmdstat rpop]
            assert_match  {*calls=100,*} [cmdstat sadd]
            assert_match  {*calls=100,*} [cmdstat hset]
            assert_match  {*calls=100,*} [cmdstat spop]
            assert_match  {*calls=100,*} [cmdstat zadd]
            assert_match  {*calls=100,*} [cmdstat zpopmin]
            assert_match  {*calls=400,*} [cmdstat lrange]
            assert_match  {*calls=100,*} [cmdstat mset]
            # assert one of the non benchmarked commands is not present
            assert_match {} [cmdstat rpoplpush]
        }

        test {benchmark: multi-thread set,get} {
            set cmd [valkeybenchmark $master_host $master_port "--threads 10 -c 5 -n 10 -t set,get"]
            common_bench_setup $cmd
            default_set_get_checks

            # ensure only one key was populated
            assert_equal  {keys=1} [regexp -inline {keys=[\d]*} [r info keyspace]]
        }

        test {benchmark: pipelined full set,get} {
            set cmd [valkeybenchmark $master_host $master_port "-P 5 -c 10 -n 10010 -t set,get"]
            common_bench_setup $cmd
            assert_match  {*calls=10010,*} [cmdstat set]
            assert_match  {*calls=10010,*} [cmdstat get]
            # assert one of the non benchmarked commands is not present
            assert_match  {} [cmdstat lrange]

            # ensure only one key was populated
            assert_equal  {keys=1} [regexp -inline {keys=[\d]*} [r info keyspace]]
        }

        test {benchmark: arbitrary command} {
            set cmd [valkeybenchmark $master_host $master_port "-c 5 -n 150 INCRBYFLOAT mykey 10.0"]
            common_bench_setup $cmd
            assert_match  {*calls=150,*} [cmdstat incrbyfloat]
            # assert one of the non benchmarked commands is not present
            assert_match  {} [cmdstat get]

            # ensure only one key was populated
            assert_equal  {keys=1} [regexp -inline {keys=[\d]*} [r info keyspace]]
        }

        test {benchmark: arbitrary command sequence} {
            set cmd [valkeybenchmark $master_host $master_port "-n 12 -- incr foo ; 3 incr bar"]
            common_bench_setup $cmd
            assert_equal 3 [r get foo]
            assert_equal 9 [r get bar]
            assert_match  {*calls=12,*} [cmdstat incr]
        }

        test {benchmark: arbitrary command with data placeholder} {
            set cmd [valkeybenchmark $master_host $master_port "-n 1 -d 42 -- set k value:__data__"]
            common_bench_setup $cmd
            puts [r get k]
            assert_equal 48 [r strlen k]
        }

        test {benchmark: keyspace length} {
            set cmd [valkeybenchmark $master_host $master_port "-r 50 -t set -n 1000"]
            common_bench_setup $cmd
            assert_match  {*calls=1000,*} [cmdstat set]
            # assert one of the non benchmarked commands is not present
            assert_match  {} [cmdstat get]

            # ensure the keyspace has the desired size
            assert_equal  {keys=50} [regexp -inline {keys=[\d]*} [r info keyspace]]
        }

        test {benchmark: keyspace covered by sequential option} {
            set cmd [valkeybenchmark $master_host $master_port "-r 50 -t set -n 50 --sequential"]
            common_bench_setup $cmd
            assert_match  {*calls=50,*} [cmdstat set]

            # ensure the keyspace has the desired size
            assert_equal  {keys=50} [regexp -inline {keys=[\d]*} [r info keyspace]]
        }

        test {benchmark: multiple independent sequential replacements} {
            set cmd [valkeybenchmark $master_host $master_port "-r 50 -n 1000 --sequential -- set j__rand_int__ rain ; set k__rand_1st__ rain"]
            common_bench_setup $cmd
            assert_match  {*calls=1000,*} [cmdstat set]
            
            # ensure the keyspace has the desired size
            assert_equal  {keys=100} [regexp -inline {keys=[\d]*} [r info keyspace]]
        }

        test {benchmark: multiple occurrences of first placeholder have different values} {
            set cmd [valkeybenchmark $master_host $master_port "-r 100 -n 100 --sequential -- set rain__rand_int__ rain__rand_int__"]
            common_bench_setup $cmd
            assert_match  {*calls=100,*} [cmdstat set]
            
            # Each command takes two sequential values, so keys count by twos
            assert_equal  {keys=50} [regexp -inline {keys=[\d]*} [r info keyspace]]

            # randomly check some keys
            for {set i 0} {$i < 10} {incr i} {
                set key [r randomkey]
                assert {$key ne [r get $key]}
            }
        }

        test {benchmark: besides first placeholder, multiple placeholder occurrences have same value} {
            set cmd [valkeybenchmark $master_host $master_port "-r 100 -n 100 -P 5 --sequential -- set rain__rand_1st__ rain__rand_1st__"]
            common_bench_setup $cmd
            assert_match  {*calls=100,*} [cmdstat set]
            
            # Each command is handled separately regardness of pipelining
            assert_equal  {keys=100} [regexp -inline {keys=[\d]*} [r info keyspace]]

            # randomly check some keys
            for {set i 0} {$i < 10} {incr i} {
                set key [r randomkey]
                assert_equal $key [r get $key]
            }
        }

        test {benchmark: multiple placeholder occurrences have same value} {
            set cmd [valkeybenchmark $master_host $master_port "-r 30000000 -n 20 -- set rain__rand_int__ rain__rand_1st__"]
            common_bench_setup $cmd
            assert_match  {*calls=20,*} [cmdstat set]

            # randomly check some keys
            set different_count 0
            for {set i 0} {$i < 10} {incr i} {
                set key [r randomkey]
                set value [r get $key]
                if {$key ne $value} {
                    incr different_count
                }
            }
            assert {$different_count > 0}
        }

        test {benchmark: sequential zadd results in expected number of keys} {
            set cmd [valkeybenchmark $master_host $master_port "-r 50 -n 50 --sequential -t zadd"]
            common_bench_setup $cmd
            assert_match  {*calls=50,*} [cmdstat zadd]

            # ensure the keyspace has the desired size
            assert_equal  {keys=1} [regexp -inline {keys=[\d]*} [r info keyspace]]
            assert_match  {50} [r zcard myzset]
        }

        test {benchmark: clients idle mode should return error when reached maxclients limit} {
            set cmd [valkeybenchmark $master_host $master_port "-c 10 -I"]
            set original_maxclients [lindex [r config get maxclients] 1]
            r config set maxclients 5
            catch { exec {*}$cmd } error
            assert_match "*Error*" $error
            r config set maxclients $original_maxclients
        }

        test {benchmark: read last argument from stdin} {
            set base_cmd [valkeybenchmark $master_host $master_port "-x -n 10 set key"]
            set cmd "printf arg | $base_cmd"
            common_bench_setup $cmd
            r get key
        } {arg}

        # tls specific tests
        if {$::tls} {
            test {benchmark: specific tls-ciphers} {
                set cmd [valkeybenchmark $master_host $master_port "-r 50 -t set -n 1000 --tls-ciphers \"DEFAULT:-AES128-SHA256\""]
                common_bench_setup $cmd
                assert_match  {*calls=1000,*} [cmdstat set]
                # assert one of the non benchmarked commands is not present
                assert_match  {} [cmdstat get]
            }

            test {benchmark: tls connecting using URI with authentication set,get} {
                r config set primaryauth pass
                set cmd [valkeybenchmarkuriuserpass $master_host $master_port "default" pass "-c 5 -n 10 -t set,get"]
                common_bench_setup $cmd
                default_set_get_checks
            }

            test {benchmark: specific tls-ciphersuites} {
                r flushall
                r config resetstat
                set ciphersuites_supported 1
                set cmd [valkeybenchmark $master_host $master_port "-r 50 -t set -n 1000 --tls-ciphersuites \"TLS_AES_128_GCM_SHA256\""]
                if {[catch { exec {*}$cmd } error]} {
                    set first_line [lindex [split $error "\n"] 0]
                    if {[string match "*Invalid option*" $first_line]} {
                        set ciphersuites_supported 0
                        if {$::verbose} {
                            puts "Skipping test, TLSv1.3 not supported."
                        }
                    } else {
                        puts [colorstr red "valkey-benchmark non zero code. first line: $first_line"]
                        fail "valkey-benchmark non zero code. first line: $first_line"
                    }
                }
                if {$ciphersuites_supported} {
                    assert_match  {*calls=1000,*} [cmdstat set]
                    # assert one of the non benchmarked commands is not present
                    assert_match  {} [cmdstat get]
                }
            }
        }
    }
}
