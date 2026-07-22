source tests/support/benchmark.tcl
source tests/support/cli.tcl

proc cmdstat {cmd} {
    return [cmdrstat $cmd r]
}

# common code to reset stats, flush the db and run valkey-benchmark
proc common_bench_setup {cmd} {
    r config resetstat
    r flushall
    if {[catch { exec {*}$cmd 2>@1 } output]} {
        set first_line [lindex [split $output "\n"] 0]
        puts [colorstr red "valkey-benchmark non zero code, the output is: $output"]
        fail "valkey-benchmark non zero code. first line: $first_line"
    }
    return $output
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

        test {benchmark: dataset CSV with quoted field values containing commas} {
            # A value with an embedded comma must be quoted and parse correctly
            set csv_data "id,title\n1,\"Book, Part 1\"\n"
            set csv_file [tmpfile "quoted.csv"]
            set fd [open $csv_file w]
            puts $fd $csv_data
            close $fd

            set cmd [valkeybenchmark $master_host $master_port \
                "--dataset $csv_file -n 2 -r 10 -- SET doc:__rand_int__ \"__field:title__\""]
            common_bench_setup $cmd

            # Verify the quoted comma-containing value was stored intact
            set keys [r keys "doc:*"]
            assert {[llength $keys] > 0}
            foreach key $keys {
                assert_equal "Book, Part 1" [r get $key]
            }
            file delete $csv_file
        }

        test {benchmark: dataset CSV with field placeholders} {
            # Create test CSV dataset
            set csv_data "title,content,author\nTest Title 1,Test Content 1,Author 1\nTest Title 2,Test Content 2,Author 2"
            set csv_file [tmpfile "dataset.csv"]
            set fd [open $csv_file w]
            puts $fd $csv_data
            close $fd

            set cmd [valkeybenchmark $master_host $master_port "--dataset $csv_file -n 4 -r 10 -- HSET doc:__rand_int__ title \"__field:title__\" content \"__field:content__\""]
            common_bench_setup $cmd
            assert_match  {*calls=4,*} [cmdstat hset]
            
            # Verify field data was inserted correctly
            set keys [r keys "doc:*"]
            assert {[llength $keys] > 0}
            set sample_key [lindex $keys 0]
            set title [r hget $sample_key title]
            set content [r hget $sample_key content]
            assert {$title eq "Test Title 1" || $title eq "Test Title 2"}
            assert {$content eq "Test Content 1" || $content eq "Test Content 2"}
            
            file delete $csv_file
        }

        test {benchmark: dataset with maxdocs limit} {
            # Create test dataset with multiple rows
            set csv_data "name,value\nitem1,value1\nitem2,value2\nitem3,value3\nitem4,value4"
            set csv_file [tmpfile "dataset.csv"]
            set fd [open $csv_file w]
            puts $fd $csv_data
            close $fd

            set cmd [valkeybenchmark $master_host $master_port "--dataset $csv_file --maxdocs 2 -n 4 -r 10 -- SET item:__rand_int__ \"__field:value__\""]
            common_bench_setup $cmd
            assert_match  {*calls=4,*} [cmdstat set]
            
            # Should only use first 2 documents due to maxdocs limit
            set keys [r keys "item:*"]
            assert {[llength $keys] > 0}
            
            # Verify ALL keys only contain values from first 2 documents
            set unique_values {}
            foreach key $keys {
                set value [r get $key]
                assert {$value eq "value1" || $value eq "value2"}
                if {[lsearch $unique_values $value] == -1} {
                    lappend unique_values $value
                }
            }
            
            file delete $csv_file
        }

        test {benchmark: dataset error handling - invalid field} {
            set csv_data "name,value\nitem1,value1"
            set csv_file [tmpfile "dataset.csv"]
            set fd [open $csv_file w]
            puts $fd $csv_data
            close $fd

            set cmd [valkeybenchmark $master_host $master_port "--dataset $csv_file -n 1 -- SET item:__rand_int__ \"__field:invalid_field__\""]
            
            # Should fail with invalid field error
            if {[catch { exec {*}$cmd } error]} {
                assert_match "*not found in dataset fields*" $error
            } else {
                fail "Expected error for invalid field placeholder"
            }
            
            file delete $csv_file
        }

        test {benchmark: dataset TSV with field placeholders} {
            # Create test TSV dataset (tab-separated values)
            set tsv_data "name\tvalue\tcount\nitem1\tvalue1\t100\nitem2\tvalue2\t200"
            set tsv_file [tmpfile "dataset.tsv"]
            set fd [open $tsv_file w]
            puts $fd $tsv_data
            close $fd

            set cmd [valkeybenchmark $master_host $master_port "--dataset $tsv_file -n 4 -r 10 -- HSET tsv_doc:__rand_int__ name \"__field:name__\" value \"__field:value__\" count __field:count__"]
            common_bench_setup $cmd
            assert_match  {*calls=4,*} [cmdstat hset]
            
            # Verify TSV field data was inserted correctly
            set keys [r keys "tsv_doc:*"]
            assert {[llength $keys] > 0}
            set sample_key [lindex $keys 0]
            set name [r hget $sample_key name]
            set value [r hget $sample_key value]
            set count [r hget $sample_key count]
            assert {$name eq "item1" || $name eq "item2"}
            assert {$value eq "value1" || $value eq "value2"}
            assert {$count eq "100" || $count eq "200"}
            
            file delete $tsv_file
        }

        test {benchmark: dataset with maxdocs larger than available documents} {
            # Create test dataset with only 2 rows but request maxdocs=5
            set csv_data "name,value\nitem1,value1\nitem2,value2"
            set csv_file [tmpfile "dataset.csv"]
            set fd [open $csv_file w]
            puts $fd $csv_data
            close $fd

            set cmd [valkeybenchmark $master_host $master_port "--dataset $csv_file --maxdocs 5 -n 4 -r 10 -- SET item:__rand_int__ \"__field:value__\""]
            common_bench_setup $cmd
            assert_match  {*calls=4,*} [cmdstat set]
            
            # Should gracefully use all available documents (2), cycling through them
            set keys [r keys "item:*"]
            assert {[llength $keys] > 0}
            
            # All values should still be only from available documents
            foreach key $keys {
                set value [r get $key]
                assert {$value eq "value1" || $value eq "value2"}
            }
            
            file delete $csv_file
        }

        test {benchmark: mixed placeholders - dataset fields and rand placeholders} {
            # Test combining __field:name__ with __rand_int__ placeholders
            set csv_data "category,description\nuser,User Management\norder,Order Processing"
            set csv_file [tmpfile "dataset.csv"]
            set fd [open $csv_file w]
            puts $fd $csv_data
            close $fd

            set cmd [valkeybenchmark $master_host $master_port "--dataset $csv_file -n 6 -r 100 -- HSET mixed:__rand_int__ category \"__field:category__\" desc \"__field:description__\" score __rand_1st__"]
            common_bench_setup $cmd
            assert_match  {*calls=6,*} [cmdstat hset]
            
            # Verify both field and random placeholders work together
            set keys [r keys "mixed:*"]
            assert {[llength $keys] > 0}
            set sample_key [lindex $keys 0]
            set category [r hget $sample_key category]
            set desc [r hget $sample_key desc]
            set score [r hget $sample_key score]
            
            # Field placeholders should contain dataset values
            assert {$category eq "user" || $category eq "order"}
            assert {$desc eq "User Management" || $desc eq "Order Processing"}
            
            # Random placeholder should be a 12-digit number
            assert {[string length $score] == 12}
            assert {[string is digit $score]}
            
            file delete $csv_file
        }

        test {benchmark: dataset mode requires field placeholders} {
            set csv_data "name,value\nitem1,value1\nitem2,value2"
            set csv_file [tmpfile "dataset.csv"]
            set fd [open $csv_file w]
            puts $fd $csv_data
            close $fd

            # Dataset mode should require field placeholders in the command
            set cmd [valkeybenchmark $master_host $master_port "--dataset $csv_file -n 10 -r 10 -t set"]
            
            # Should fail with error about missing field placeholders
            if {[catch { exec {*}$cmd } error]} {
                assert_match "*Dataset mode requires a command with field placeholders*" $error
            } else {
                fail "Expected error for dataset mode without field placeholders"
            }
            
            file delete $csv_file
        }

        test {benchmark: dataset with --csv produces clean CSV only} {
            # Create test CSV dataset
            set csv_data "name,value\ntest1,val1\ntest2,val2"
            set csv_file [tmpfile "csv_clean.csv"]
            set fd [open $csv_file w]
            puts $fd $csv_data
            close $fd

            set cmd [valkeybenchmark $master_host $master_port "--dataset $csv_file --csv -n 4 -r 10 -- SET item:__rand_int__ \"__field:value__\""]
            set output [common_bench_setup $cmd]
            
            # Should NOT contain dataset diagnostic messages (those go to stderr)
            assert_no_match "*Dataset:*documents*" $output
            assert_no_match "*Loading*dataset*" $output
            
            # Should contain valid CSV format
            assert_match "*\"*\",\"*\",\"*\",\"*\",\"*\",\"*\",\"*\",\"*\"*" $output
            
            file delete $csv_file
        }

        test {benchmark: sequential zadd results in expected number of keys} {
            set cmd [valkeybenchmark $master_host $master_port "-r 50 -n 50 --sequential -t zadd"]
            common_bench_setup $cmd
            assert_match  {*calls=50,*} [cmdstat zadd]

            # ensure the keyspace has the desired size
            assert_equal  {keys=1} [regexp -inline {keys=[\d]*} [r info keyspace]]
            assert_match  {50} [r zcard myzset]
        }

        test {benchmark: warmup and duration are cumulative} {
            set start_time [clock clicks -millisec]
            set cmd [valkeybenchmark $master_host $master_port "-r 5 --warmup 1 --duration 1 -t set"]
            set output [common_bench_setup $cmd]
            set end_time [clock clicks -millisec]

            # Verify total duration was at least 2 seconds
            set elapsed [expr {($end_time - $start_time)/1000.0}]
            assert {$elapsed >= 2}

            # Check reported duration
            lassign [regexp -inline {(\d+) requests completed in ([\d.]+) seconds} $output] -> requests duration
            assert {$duration < 2.0 && $duration >= 1.0}
        }

        test {benchmark: warmup can be used with request count} {
            set start_time [clock clicks -millisec]
            set cmd [valkeybenchmark $master_host $master_port "-r 5 --warmup 1 -n 100 -t set"]
            set output [common_bench_setup $cmd]
            set end_time [clock clicks -millisec]

            # Verify total duration was at least 1 seconds
            set elapsed [expr {($end_time - $start_time)/1000.0}]
            assert {$elapsed >= 1}

            # Check reported duration and command count
            lassign [regexp -inline {(\d+) requests completed in ([\d.]+) seconds} $output] -> requests duration
            assert {$duration < 1.0}
            assert {$requests >= 100 && $requests < 150}
        }

        test {benchmark: -n and --duration are mutually exclusive} {
            set cmd [valkeybenchmark $master_host $master_port "-r 5 -n 5 --duration 1 -t set"]
            catch { exec {*}$cmd } error
            assert_match "*Options -n and --duration are mutually exclusive*" $error
        }

        test {benchmark: warmup applies to all tests in multi-test run} {
            set start_time [clock clicks -millisec]
            set cmd [valkeybenchmark $master_host $master_port "-r 5 --warmup 2 -n 50 -t set,get,incr"]
            set output [common_bench_setup $cmd]
            set end_time [clock clicks -millisec]

            # Verify total duration includes warmup for all 3 tests (at least 6 seconds)
            set elapsed [expr {($end_time - $start_time)/1000.0}]
            assert {$elapsed >= 6}

            # Verify all tests ran - with warmup, we expect more than 50 calls per command
            # since warmup commands are also counted in server stats
            assert_match  {*calls=*} [cmdstat set]
            assert_match  {*calls=*} [cmdstat get]
            assert_match  {*calls=*} [cmdstat incr]
            
            # Verify that each command was called more than the base 50 requests
            # due to warmup period adding extra requests
            set set_calls [regexp -inline {calls=(\d+)} [cmdstat set]]
            set get_calls [regexp -inline {calls=(\d+)} [cmdstat get]]
            set incr_calls [regexp -inline {calls=(\d+)} [cmdstat incr]]
            
            lassign $set_calls -> set_count
            lassign $get_calls -> get_count
            lassign $incr_calls -> incr_count
            
            assert {$set_count > 50}
            assert {$get_count > 50}
            assert {$incr_count > 50}
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

        test {benchmark: CSV output format} {
            set cmd [valkeybenchmark $master_host $master_port "-c 5 -n 10 -t set,get --csv"]
            set output [common_bench_setup $cmd]
            # CSV output should contain comma-separated values
            assert_match "*,*,*,*" $output
            # Should not contain the usual formatted output
            assert_no_match "*requests per second*" $output
            # Should not contain carriage returns or progress indicators
            assert_no_match "*\r*" $output
            assert_no_match "*rps=*" $output
            default_set_get_checks
        }

        test {benchmark: quiet mode} {
            set cmd [valkeybenchmark $master_host $master_port "-c 5 -n 10 -t set,get -q"]
            set output [common_bench_setup $cmd]
            # Quiet mode should only show query/sec values (case-insensitive)
            assert {[string match -nocase "*requests per second*" $output]}
            # Should not contain detailed latency information (case-insensitive)
            assert {![string match -nocase "*distribution*" $output]}
            assert {![string match -nocase "*percentile*" $output]}
            default_set_get_checks
        }

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

# Cluster mode tests
tags {"benchmark network external:skip cluster"} {
    start_cluster 3 0 {tags {"external:skip"}} {
        test "Cluster benchmark: dataset with {tag} validates replacement" {
            # Create test CSV dataset
            set csv_data "id,name\n1,Item1\n2,Item2"
            set csv_file [tmpfile "cluster_ds.csv"]
            set fd [open $csv_file w]
            puts $fd $csv_data
            close $fd

            # Get cluster node
            set host [srv 0 host]
            set port [srv 0 port]

            # Run with --cluster, {tag}, and dataset
            set cmd [valkeybenchmark $host $port "--cluster \
                --dataset $csv_file -n 2 -r 100 --sequential \
                HSET doc:\{tag\}:__rand_int__ name __field:name__"]
            
            if {[catch { exec {*}$cmd 2>@1 } output]} {
                if {![string match "*HSET*" $output]} {
                    fail "Benchmark failed: $output"
                }
            }

            # Collect all keys from all nodes
            set all_keys {}
            for {set id 0} {$id < 3} {incr id} {
                set keys [R $id keys "doc:*"]
                foreach key $keys {
                    lappend all_keys [list $id $key]
                }
            }
            
            # Should have exactly 2 keys total
            assert {[llength $all_keys] == 2}
            
            # Verify each key
            foreach key_info $all_keys {
                lassign $key_info node_id key
                # {tag} replaced (not literal)
                assert {![string match "*\{tag\}*" $key]}
                # Has cluster hash tag format
                assert {[string match "doc:\{*\}:*" $key]}
                # __rand_int__ replaced with 12 digits
                assert {[regexp {:\d{12}$} $key]}
                # Dataset field inserted
                set name [R $node_id hget $key name]
                assert {$name in {Item1 Item2}}
            }
            
            file delete $csv_file
        }
    }
}
