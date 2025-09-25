proc get_function_code {args} {
    return [format "#!%s name=%s\nserver.register_function('%s', function(KEYS, ARGV)\n %s \nend)" [lindex $args 0] [lindex $args 1] [lindex $args 2] [lindex $args 3]]
}

tags {"check-rdb external:skip logreqres:skip"} {
    test {Check old valid RDB} {
        catch {
            exec src/valkey-check-rdb tests/assets/encodings.rdb
        } result
        assert_match {*\[offset ???\] \\o/ RDB looks OK! \\o/*} $result
    }

    test {Check foreign RDB without unknown data} {
        catch {
            exec src/valkey-check-rdb tests/assets/encodings-rdb12.rdb
        } result
        assert_match {*\[offset ?\] Foreign RDB version 12 detected*} $result
        assert_match {*\[offset ???\] \\o/ RDB looks OK, but loading requires config 'rdb-version-check relaxed'*} $result
    }

    test {Check foreign RDB with unknown data} {
        catch {
            exec src/valkey-check-rdb tests/assets/encodings-rdb75-unknown-types.rdb
        } result
        assert_match {*\[offset ?\] Foreign RDB version 75 detected*} $result
        assert_match {*--- RDB ERROR DETECTED ---*} $result
        assert_match {*\[offset ??\] Unknown object type 150 in RDB file with foreign version 75*} $result
        assert_no_match {*RDB looks OK*} $result
    }

    test {Check future RDB without unknown data} {
        catch {
            exec src/valkey-check-rdb tests/assets/encodings-rdb987.rdb
        } result
        assert_match {*\[offset ?\] Future RDB version 987 detected*} $result
        assert_match {*\[offset ???\] \\o/ RDB looks OK, but loading requires config 'rdb-version-check relaxed'*} $result
    }

    test {Check future RDB with unknown data} {
        catch {
            exec src/valkey-check-rdb tests/assets/encodings-rdb987-unknown-types.rdb
        } result
        assert_match {*\[offset ?\] Future RDB version 987 detected*} $result
        assert_match {*--- RDB ERROR DETECTED ---*} $result
        assert_match {*\[offset ??\] Unknown object type 150 in RDB file with future version 987*} $result
        assert_no_match {*RDB looks OK*} $result
    }
}

tags {"check-rdb network external:skip logreqres:skip"} {
    start_server {} {
        test "test valkey-check-rdb stats with empty RDB" {
            r flushall
            r save
            set dump_rdb [file join [lindex [r config get dir] 1] dump.rdb]
            catch {
                exec src/valkey-check-rdb $dump_rdb --stats --format info
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
                exec src/valkey-check-rdb $dump_rdb
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
                exec src/valkey-check-rdb $dump_rdb --stats --format info
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
