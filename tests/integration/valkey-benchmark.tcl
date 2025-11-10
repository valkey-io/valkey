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

        test {benchmark: dataset XML with field placeholders} {
            # Create test XML dataset matching Wikipedia structure
            set xml_data "<doc><title>XML Title 1</title><abstract>XML Abstract 1</abstract><url>http://example1.com</url><links><sublink><anchor>test1</anchor><link>http://test1.com</link></sublink></links></doc>\n<doc><title>XML Title 2</title><abstract>XML Abstract 2</abstract><url>http://example2.com</url><links><sublink><anchor>test2</anchor><link>http://test2.com</link></sublink></links></doc>"
            set xml_file [tmpfile "dataset.xml"]
            set fd [open $xml_file w]
            puts $fd $xml_data
            close $fd

            set cmd [valkeybenchmark $master_host $master_port "--dataset $xml_file --xml-root-element doc -n 4 -r 10 -- HSET xml_doc:__rand_int__ title \"__field:title__\" abstract \"__field:abstract__\""]
            common_bench_setup $cmd
            assert_match  {*calls=4,*} [cmdstat hset]
            
            # Verify XML field data was inserted correctly
            set keys [r keys "xml_doc:*"]
            assert {[llength $keys] > 0}
            set sample_key [lindex $keys 0]
            set title [r hget $sample_key title]
            set abstract [r hget $sample_key abstract]
            assert {$title eq "XML Title 1" || $title eq "XML Title 2"}
            assert {$abstract eq "XML Abstract 1" || $abstract eq "XML Abstract 2"}
            
            file delete $xml_file
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

        test {benchmark: XML dataset missing root element error} {
            # Create test XML dataset
            set xml_data "<doc><title>XML Title 1</title><abstract>XML Abstract 1</abstract></doc>"
            set xml_file [tmpfile "dataset.xml"]
            set fd [open $xml_file w]
            puts $fd $xml_data
            close $fd

            # Should fail without --xml-root-element parameter
            set cmd [valkeybenchmark $master_host $master_port "--dataset $xml_file -n 1 -- SET xml:__rand_int__ \"__field:title__\""]
            
            if {[catch { exec {*}$cmd } error]} {
                assert_match "*XML dataset requires --xml-root-element parameter*" $error
            } else {
                fail "Expected error for XML dataset without --xml-root-element"
            }
            
            file delete $xml_file
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
