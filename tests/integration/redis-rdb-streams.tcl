# Copyright (c) Valkey Contributors
# All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause

# These are actual Redis 8.6.0 SAVE and DUMP outputs, not Valkey streams with a
# patched type byte. See tests/helpers/gen-redis-streams.tcl for regeneration.
# Only fields implemented by both servers are compared; Redis IDMP state is
# deliberately discarded with a warning.
proc assert_redis_stream_metadata {key expected} {
    set actual [r xinfo stream $key full count 0]
    assert_equal [dict get $expected entries] [r xrange $key - +]
    foreach field {length radix-tree-keys last-generated-id max-deleted-entry-id entries-added recorded-first-entry-id entries} {
        assert_equal [dict get $expected $field] [dict get $actual $field]
    }
    set groups [dict get $actual groups]
    assert_equal [llength [dict get $expected groups]] [llength $groups]
    foreach want [dict get $expected groups] got $groups {
        foreach field {name last-delivered-id entries-read lag pel-count pending} {
            assert_equal [dict get $want $field] [dict get $got $field]
        }
        set consumers [dict get $got consumers]
        assert_equal [llength [dict get $want consumers]] [llength $consumers]
        foreach wc [dict get $want consumers] gc $consumers {
            foreach field {name seen-time active-time pel-count pending} {
                assert_equal [dict get $wc $field] [dict get $gc $field]
            }
        }
    }
}

tags {"rdb stream external:skip"} {
    lassign [binary format H* [string trim [read_binary_file tests/assets/redis-8.6-streams.txt]]] expected dumps
    set path [tmpdir redis-rdb-streams]
    file copy tests/assets/redis-8.6-streams.rdb [file join $path dump.rdb]

    start_server [list overrides [list dir $path rdb-version-check relaxed] keep_persistence true] {
        r select 0
        test {Redis RDB 13 stream entries, groups, consumers and pending metadata survive import} {
            dict for {key info} $expected {
                assert_redis_stream_metadata $key $info
            }
            assert_equal 5 [r dbsize]
            binary scan [r dump idmp] c type
            assert_equal 21 $type
            assert_equal after-streams [r get sentinel]
            verify_log_message 0 "*Discarding Redis stream IDMP state*" 0
        }

        test {Imported Redis streams remain writable and persist in Valkey format} {
            assert_equal 1 [r xack plain first 2-0]
            assert_equal 8-0 [r xadd plain 8-0 f appended]
            set info [r xinfo stream plain full count 0]
            r save
            r debug reload
            assert_redis_stream_metadata plain $info
            assert_equal after-streams [r get sentinel]
        } {} {needs:debug}
    }

    start_server {overrides {rdb-version-check relaxed}} {
        test {RESTORE imports Redis 8.6 type-26 streams} {
            dict for {key hex} $dumps {
                assert_equal OK [r restore $key 0 [binary format H* $hex]]
                assert_redis_stream_metadata $key [dict get $expected $key]
            }
            verify_log_message 0 "*Discarding Redis stream IDMP state*" 0
        }

        test {RESTORE does not warn for streams without IDMP state} {
            set warnings [count_log_message 0 "Discarding Redis stream IDMP state"]
            r restore plain 0 [binary format H* [dict get $dumps plain]] replace
            assert_equal $warnings [count_log_message 0 "Discarding Redis stream IDMP state"]
        }

        test {Strict mode still rejects Redis RDB 13 DUMP payloads} {
            r config set rdb-version-check strict
            assert_error "*DUMP payload version or checksum are wrong*" {
                r restore rejected 0 [binary format H* [dict get $dumps plain]]
            }
            r config set rdb-version-check relaxed
        }

        test {Truncated Redis stream IDMP trailers fail without replacing an existing key} {
            r set protected value
            r debug set-skip-checksum-validation 1
            # Retain the Redis version/footer but truncate at every position
            # in the type-26 object, including both final counters.
            set dump [binary format H* [dict get $dumps idmp]]
            set body [string range $dump 0 end-10]
            set footer [string range $dump end-9 end]
            for {set len 1} {$len < [string length $body]} {incr len} {
                set truncated "[string range $body 0 [expr {$len - 1}]]$footer"
                assert_error "*Bad data format*" {r restore protected 0 $truncated replace}
                assert_equal value [r get protected]
            }
            r debug set-skip-checksum-validation 0
        } {OK} {needs:debug}

        test {Redis IDMP metadata validates integer encodings, counts and payload boundaries} {
            r debug set-skip-checksum-validation 1
            set dump [binary format H* [dict get $dumps no-entries]]
            # The fixture ends with duration=100, maxsize=100, zero producers,
            # zero added/duplicate counters (7 bytes), then the 10-byte footer.
            set prefix [string range $dump 0 end-17]
            set footer [string range $dump end-9 end]
            r set protected value
            foreach trailer {
                004064000000
                80000151814064000000
                406400000000
                40646711000000
                40644064c00000
                4064406481ffffffffffffffff
                4064406401017081ffffffffffffffff
                4064406400000000
            } {
                set corrupt "$prefix[binary format H* $trailer]$footer"
                assert_error "*Bad data format*" {r restore protected 0 $corrupt replace}
                assert_equal value [r get protected]
            }

            # Full-width counters are legal uint64_t values, not RDB_LENERR.
            set warnings [count_log_message 0 "Discarding Redis stream IDMP state"]
            set trailer [binary format H* 406440640081ffffffffffffffff81ffffffffffffffff]
            assert_equal OK [r restore history 0 "$prefix$trailer$footer"]
            assert_redis_stream_metadata history [dict get $expected no-entries]
            assert_equal [expr {$warnings + 1}] [count_log_message 0 "Discarding Redis stream IDMP state"]

            # Type 26 must never acquire a meaning in Valkey's own namespace.
            set native_footer "[binary format cc 81 0][string repeat \x00 8]"
            assert_error "*Bad data format*" {r restore protected 0 "$prefix$trailer$native_footer" replace}
            assert_equal value [r get protected]

            # Do not reinterpret conflicting Redis hash types as Valkey hashes
            # or Path Hashes when loading a foreign dump.
            foreach type {22 23} {
                set foreign "[binary format c $type][string range $dump 1 end]"
                assert_error "*Unsupported foreign data type*" {r restore protected 0 $foreign replace}
                assert_equal value [r get protected]
            }
            r debug set-skip-checksum-validation 0
        } {OK} {needs:debug}
    }

    test {valkey-check-rdb understands Redis RDB 13 streams} {
        set result [exec $::VALKEY_CHECK_RDB_BIN tests/assets/redis-8.6-streams.rdb]
        assert_match "*RDB looks OK, but loading requires config 'rdb-version-check relaxed'*" $result
    }

    test {Redis stream type 26 is rejected outside the foreign RDB namespace} {
        set data [read_binary_file tests/assets/redis-8.6-streams.rdb]
        set filename [tmpfile foreign-stream-type.rdb]
        foreach header {REDIS0011 VALKEY081} {
            write_binary_file $filename "$header[string range $data 9 end]"
            assert_equal 1 [catch {exec $::VALKEY_CHECK_RDB_BIN $filename} result]
            assert_match "*Invalid object type: 26*" $result
        }
    }
}
