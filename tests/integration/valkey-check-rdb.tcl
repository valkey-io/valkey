proc get_function_code {args} {
    return [format "#!%s name=%s\nserver.register_function('%s', function(KEYS, ARGV)\n %s \nend)" [lindex $args 0] [lindex $args 1] [lindex $args 2] [lindex $args 3]]
}

tags {"check-rdb external:skip logreqres:skip"} {
    test {Check old valid RDB} {
        catch {
            exec $::VALKEY_CHECK_RDB_BIN tests/assets/encodings.rdb
        } result
        assert_match {*\[offset ???\] \\o/ RDB looks OK! \\o/*} $result
    }

    test {Check foreign RDB without unknown data} {
        catch {
            exec $::VALKEY_CHECK_RDB_BIN tests/assets/encodings-rdb12.rdb
        } result
        assert_match {*\[offset ?\] Foreign RDB version 12 detected*} $result
        assert_match {*\[offset ???\] \\o/ RDB looks OK, but loading requires config 'rdb-version-check relaxed'*} $result
    }

    test {Check foreign RDB with unknown data} {
        catch {
            exec $::VALKEY_CHECK_RDB_BIN tests/assets/encodings-rdb75-unknown-types.rdb
        } result
        assert_match {*\[offset ?\] Foreign RDB version 75 detected*} $result
        assert_match {*--- RDB ERROR DETECTED ---*} $result
        assert_match {*\[offset ??\] Unknown object type 150 in RDB file with foreign version 75*} $result
        assert_no_match {*RDB looks OK*} $result
    }

    test {Check future RDB without unknown data} {
        catch {
            exec $::VALKEY_CHECK_RDB_BIN tests/assets/encodings-rdb987.rdb
        } result
        assert_match {*\[offset ?\] Future RDB version 987 detected*} $result
        assert_match {*\[offset ???\] \\o/ RDB looks OK, but loading requires config 'rdb-version-check relaxed'*} $result
    }

    test {Check future RDB with unknown data} {
        catch {
            exec $::VALKEY_CHECK_RDB_BIN tests/assets/encodings-rdb987-unknown-types.rdb
        } result
        assert_match {*\[offset ?\] Future RDB version 987 detected*} $result
        assert_match {*--- RDB ERROR DETECTED ---*} $result
        assert_match {*\[offset ??\] Unknown object type 150 in RDB file with future version 987*} $result
        assert_no_match {*RDB looks OK*} $result
    }
}

tags {"check-rdb network external:skip logreqres:skip"} {
    start_server {} {
        test "valkey-check-rdb validates the contents of an LZ4-compressed RDB" {
            r config set rdbcompression lz4
            with_cleanup {
                r flushall
                r set lz4:key [string repeat "payload " 200]
                r save

                set dump_rdb [file join [lindex [r config get dir] 1] dump.rdb]
                set failed [catch {
                    exec $::VALKEY_CHECK_RDB_BIN $dump_rdb --stats --format info
                } result]

                assert_equal 0 $failed
                assert_match {*RDB looks OK!*} $result
                assert_match {*\[logical offset *, physical offset *\] Logical RDB CRC64 skipped for streaming-compressed input*} $result
                assert_match {*type.string.keys.total:1*} $result
                assert_no_match {*Checksum OK*} $result
            } {
                catch {r config set rdbcompression yes}
            }
        }

        test "valkey-check-rdb rejects an incompatible VCS envelope" {
            r config set rdbcompression lz4
            set dir [lindex [r config get dir] 1]
            set incompatible_rdb [file join $dir incompatible-vcs.rdb]
            with_cleanup {
                r flushall
                r set lz4:incompatible payload
                r save

                set dump_rdb [file join $dir dump.rdb]
                set data [read_binary_file $dump_rdb]
                set data [string replace $data 3 3 [binary format c 2]]
                write_binary_file $incompatible_rdb $data

                set failed [catch {
                    exec $::VALKEY_CHECK_RDB_BIN $incompatible_rdb
                } result]

                assert_equal 1 $failed
                assert_match {*Invalid or unsupported RDB stream envelope*} $result
                assert_no_match {*RDB looks OK*} $result
            } {
                file delete -force $incompatible_rdb
                catch {r config set rdbcompression yes}
            }
        }

        test "valkey-check-rdb rejects a compressed RDB with a truncated frame trailer" {
            r config set rdbcompression lz4
            set dir [lindex [r config get dir] 1]
            set truncated_rdb [file join $dir truncated-vcs.rdb]
            with_cleanup {
                r flushall
                r set lz4:truncated [string repeat "payload " 200]
                r save

                set dump_rdb [file join $dir dump.rdb]
                set data [read_binary_file $dump_rdb]
                write_binary_file $truncated_rdb [string range $data 0 end-1]

                set failed [catch {
                    exec $::VALKEY_CHECK_RDB_BIN $truncated_rdb
                } result]

                assert_equal 1 $failed
                assert_match {*\[logical offset *, physical offset *\] Compressed RDB stream did not end cleanly*} $result
                assert_no_match {*RDB looks OK*} $result
            } {
                file delete -force $truncated_rdb
                catch {r config set rdbcompression yes}
            }
        }

        test "valkey-check-rdb rejects a compressed RDB truncated mid-frame" {
            r config set rdbcompression lz4
            set dir [lindex [r config get dir] 1]
            set midframe_rdb [file join $dir midframe-vcs.rdb]
            with_cleanup {
                r flushall
                r set lz4:midframe [string repeat "payload " 200]
                r save

                set dump_rdb [file join $dir dump.rdb]
                set data [read_binary_file $dump_rdb]
                # Cut mid-frame rather than dropping the trailer: a file has no
                # more bytes coming, so this is corruption.
                set keep [expr {[string length $data] * 6 / 10}]
                write_binary_file $midframe_rdb [string range $data 0 [expr {$keep - 1}]]

                set failed [catch {
                    exec $::VALKEY_CHECK_RDB_BIN $midframe_rdb
                } result]

                assert_equal 1 $failed
                assert_match {*Corrupt compressed RDB stream*} $result
                assert_no_match {*RDB looks OK*} $result
            } {
                file delete -force $midframe_rdb
                catch {r config set rdbcompression yes}
            }
        }

        test "valkey-check-rdb ignores trailing data after a compressed RDB" {
            r config set rdbcompression lz4
            set dir [lindex [r config get dir] 1]
            set trailing_rdb [file join $dir trailing-vcs.rdb]
            with_cleanup {
                r flushall
                r set lz4:trailing payload
                r save

                set dump_rdb [file join $dir dump.rdb]
                set data [read_binary_file $dump_rdb]
                write_binary_file $trailing_rdb "${data}trailing-data"

                set failed [catch {
                    exec $::VALKEY_CHECK_RDB_BIN $trailing_rdb
                } result]

                assert_equal 0 $failed
                assert_match {*RDB looks OK*} $result
            } {
                file delete -force $trailing_rdb
                catch {r config set rdbcompression yes}
            }
        }

        test "valkey-check-rdb rejects corruption in a compressed RDB stream" {
            set dir [lindex [r config get dir] 1]
            set corrupt_rdb [file join $dir corrupt-vcs.rdb]
            r config set rdbcompression lz4
            with_cleanup {
                r flushall
                for {set i 0} {$i < 100} {incr i} {
                    r set "lz4:corrupt:$i" [string repeat "payload-$i " 100]
                }
                r save

                set dump_rdb [file join $dir dump.rdb]
                set data [read_binary_file $dump_rdb]
                set offset [expr {[string length $data] / 2}]
                binary scan [string index $data $offset] cu value
                set data [string replace $data $offset $offset [binary format c [expr {$value ^ 1}]]]

                write_binary_file $corrupt_rdb $data
                set failed [catch {
                    exec $::VALKEY_CHECK_RDB_BIN $corrupt_rdb
                } result]

                assert_equal 1 $failed
                assert_match {*Corrupt compressed RDB stream*} $result
                assert_no_match {*RDB looks OK*} $result
            } {
                file delete -force $corrupt_rdb
                catch {r config set rdbcompression yes}
            }
        }

        test "test valkey-check-rdb stats with empty RDB" {
            r flushall
            r save
            set dump_rdb [file join [lindex [r config get dir] 1] dump.rdb]
            catch {
                exec $::VALKEY_CHECK_RDB_BIN $dump_rdb --stats --format info
            } result
            assert_match "*db.0.type.string.keys.total:0*" $result
            assert_match "*db.0.type.list.keys.total:0*" $result
            assert_match "*db.0.type.set.keys.total:0*" $result
            assert_match "*db.0.type.zset.keys.total:0*" $result
            assert_match "*db.0.type.hash.keys.total:0*" $result
            assert_match "*db.0.type.stream.keys.total:0*" $result
        }

        test "test valkey-check-rdb stats function" {
            set function_num 11
            for {set i 0} {$i < $function_num} {incr i} {
                r function load [get_function_code LUA "test_$i" "test_$i" {return '$i'}]
            }
            r save
            
            set dump_rdb [file join [lindex [r config get dir] 1] dump.rdb]
            catch {
                exec $::VALKEY_CHECK_RDB_BIN $dump_rdb
            } result
            assert_match "*$function_num functions*" $result
        }

        test "test valkey-check-rdb stats key space" {
            r select 0
            for {set i 10} {$i < 20} {incr i} {
                set key [string repeat "$i" 10]
                set value [string repeat "$i" 100]
                r set $key $value
            }
            r select 1
            for {set i 20} {$i < 30} {incr i} {
                set key [string repeat "$i" 10]
                for {set j 0} {$j < 5} {incr j} {
                    r lpush $key [string repeat "$i" 100]
                }
            }
            r select 2
            for {set i 30} {$i < 40} {incr i} {
                set key [string repeat "$i" 10]
                for {set j 10} {$j < 20} {incr j} {
                    r sadd $key [string repeat "$j" 100]
                }
            }
            r select 3
            for {set i 40} {$i < 50} {incr i} {
                set key [string repeat "$i" 10]
                for {set j 10} {$j < 20} {incr j} {
                    set score $j
                    set member [string repeat "$j" 100]
                    r zadd $key $score $member
                }
            }
            r select 4
            for {set i 50} {$i < 60} {incr i} {
                set key [string repeat "$i" 10]
                for {set j 10} {$j < 20} {incr j} {
                    set field [string repeat "$j" 10]
                    set field_value [string repeat "$j" 10]
                    r hset $key $field $field_value
                }
            }
            r select 5
            for {set i 60} {$i < 70} {incr i} {
                set key [string repeat "$i" 10]
                for {set j 10} {$j < 20} {incr j} {
                    set field [string repeat "$j" 10]
                    set field_value [string repeat "$j" 10]
                    r xadd $key * $field $field_value
                }
            }
            r save
            
            set dump_rdb [file join [lindex [r config get dir] 1] dump.rdb]
            catch {
                exec $::VALKEY_CHECK_RDB_BIN $dump_rdb --stats --format info
            } result

            assert_match "*db.0.type.string.keys.total:10*" $result
            assert_match "*db.1.type.list.keys.total:10*" $result
            assert_match "*db.2.type.set.keys.total:10*" $result
            assert_match "*db.3.type.zset.keys.total:10*" $result
            assert_match "*db.4.type.hash.keys.total:10*" $result
            assert_match "*db.5.type.stream.keys.total:10*" $result
        }
    }
}

tags {"check-rdb network external:skip logreqres:skip"} {
    start_server {overrides {save "" rdbchecksum no}} {
        test "valkey-check-rdb accepts compressed RDBs created with rdbchecksum no" {
            r flushall
            r config set rdbcompression lz4
            r set lz4:no-cksum [string repeat "payload " 200]
            r save

            set dump_rdb [file join [lindex [r config get dir] 1] dump.rdb]
            set failed [catch {
                exec $::VALKEY_CHECK_RDB_BIN $dump_rdb
            } result]
            assert_equal 0 $failed
            assert_match {*Logical RDB CRC64 skipped for streaming-compressed input*} $result
            assert_match {*RDB looks OK!*} $result
            assert_no_match {*Checksum OK*} $result
        }
    }
}
