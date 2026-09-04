# This file tests the loading and checking of the slot RDB. All tmpdirs need to start
# with "rdb-slot-import-" so that the test suite can clean up the temporary files.

tags {"rdb cluster external:skip"} {

# Helper: start a server that is expected to fail during RDB load, then inspect logs.
proc start_server_and_kill_it {overrides code} {
    upvar srv srv
    set ::slot_import_short_name_asan 0
    set srv [start_server [list overrides $overrides keep_persistence true]]
    uplevel 1 $code
    # Server may already be dead after a failed RDB load; skip leak checks.
    dict set srv skipleaks 1
    # Pre-fix ASan OOB is already reported by the test assertion; clear stderr
    # so kill_server does not emit a duplicate sanitizer failure.
    if {$::slot_import_short_name_asan} {
        close [open [dict get $srv stderr] w]
    }
    kill_server $srv
}

# Craft an RDB with RDB_OPCODE_SLOT_IMPORT (0xF3) whose job_name is 1 byte.
# A correct save path always writes CLUSTER_NAMELEN (40) bytes; the load path
# must reject shorter names before createSlotImportJob() memcpy's 40 bytes.
#
# Trailing checksum is written as 8 zero bytes. The loader treats cksum==0 as
# "checksum disabled", so we do not need an external CRC64 helper.
proc craft_slot_import_short_job_name_rdb {src_rdb dst_rdb} {
    set fd [open $src_rdb rb]
    fconfigure $fd -translation binary
    set data [read $fd]
    close $fd

    # Empty RDB ends with: EOF(0xFF) + 8-byte CRC64.
    binary scan [string index $data end-8] c eof_byte
    set eof_byte [expr {$eof_byte & 0xff}]
    if {$eof_byte != 0xff} {
        error "Expected RDB EOF opcode 0xFF before checksum, got $eof_byte"
    }

    set prefix [string range $data 0 end-9]
    # F3 | len=1 | 'A' | num_ranges=1 | start=0 | end=0 | FF | cksum=0
    set record [binary format H* f30141010000ff0000000000000000]
    set fd [open $dst_rdb wb]
    fconfigure $fd -translation binary
    puts -nonewline $fd $prefix
    puts -nonewline $fd $record
    close $fd
}

set gen_path [tmpdir "rdb-slot-import-gen"]
start_server [list overrides [list "dir" $gen_path "save" "" "dbfilename" "empty.rdb"] keep_persistence true] {
    test {Generate baseline empty RDB for slot-import craft} {
        r save
        assert_equal 1 [file exists [file join $gen_path empty.rdb]]
    }
}

set victim_path [tmpdir "rdb-slot-import-short-name"]
set victim_rdb [file join $victim_path dump.rdb]
craft_slot_import_short_job_name_rdb \
    [file join $gen_path empty.rdb] \
    $victim_rdb

test {valkey-check-rdb rejects short slot-import job_name} {
    catch {
        exec $::VALKEY_CHECK_RDB_BIN $victim_rdb
    } result
    assert_match {*--- RDB ERROR DETECTED ---*} $result
    assert_match {*Invalid slot import job name length*} $result
    assert_no_match {*RDB looks OK*} $result
}

start_server_and_kill_it [list \
    "dir" $victim_path \
    "dbfilename" "dump.rdb" \
    "save" "" \
    "appendonly" "no" \
    "cluster-enabled" "yes" \
    "cluster-config-file" "nodes.conf" \
] {
    test {Server rejects RDB slot-import job_name shorter than CLUSTER_NAMELEN} {
        # Before the fix (see #4207): ASan aborts in createSlotImportJob()'s
        # memcpy(..., CLUSTER_NAMELEN) on a 1-byte job_name, or a non-ASan build
        # may perform an OOB read. After the fix the loader must fail closed.
        wait_for_condition 50 100 {
            [string match {*Invalid slot import job name*} \
                [exec cat [dict get $srv stdout]]] ||
            [string match {*Short read or OOM loading DB*} \
                [exec cat [dict get $srv stdout]]] ||
            [string match {*Fatal error loading the DB*} \
                [exec cat [dict get $srv stdout]]]
        } else {
            set stdout [exec cat [dict get $srv stdout]]
            set stderr [exec cat [dict get $srv stderr]]
            if {[string match {*heap-buffer-overflow*} $stderr] ||
                [string match {*AddressSanitizer*} $stderr]} {
                set ::slot_import_short_name_asan 1
                fail "Bug reproduced: short slot-import job_name caused sanitizer OOB read during RDB load (expected until fixed)."
            }
            fail "Server did not reject short slot-import job_name RDB.\nSTDOUT:\n$stdout\nSTDERR:\n$stderr"
        }

        # Must not become ready for clients.
        assert_equal 0 [count_message_lines [dict get $srv stdout] "Ready to accept"]
    }
}

# Return the bytes of an RDB length, encoded the same way rdbSaveLen() does:
# 6 bit (1 byte), 14 bit (2 bytes) or 32 bit (0x80 + 4 bytes).
proc rdb_len_bytes {len} {
    if {$len < (1 << 6)} {
        set hex [format %02x $len]
    } elseif {$len < (1 << 14)} {
        set hex [format %02x%02x [expr {0x40 | ($len >> 8)}] [expr {$len & 0xff}]]
    } else {
        set hex [format 80%08x $len]
    }
    return [binary format H* $hex]
}

# Craft an RDB with RDB_OPCODE_SLOT_IMPORT (0xF3) holding a valid (CLUSTER_NAMELEN)
# job name and a single slot range, so the range validation is what gets exercised.
# Slot ranges must satisfy 0 <= start_slot <= end_slot < CLUSTER_SLOTS (16384).
#
# Bytes: F3 | len=40 | job_name | num_ranges=1 | start_slot | end_slot | FF | cksum=0
set range_path [tmpdir "rdb-slot-import-range"]
proc craft_slot_import_range_rdb {src_rdb dst_rdb start_slot end_slot} {
    set fd [open $src_rdb rb]
    fconfigure $fd -translation binary
    set data [read $fd]
    close $fd

    # Empty RDB ends with: EOF(0xFF) + 8-byte CRC64, replaced by our own footer.
    set prefix [string range $data 0 end-9]
    set record [binary format H* f3]
    append record [rdb_len_bytes 40][string repeat "A" 40]
    append record [rdb_len_bytes 1]
    append record [rdb_len_bytes $start_slot][rdb_len_bytes $end_slot]
    append record [binary format H* ff0000000000000000]

    set fd [open $dst_rdb wb]
    fconfigure $fd -translation binary
    puts -nonewline $fd $prefix
    puts -nonewline $fd $record
    close $fd
}

set bad_range_rdb [file join $range_path "start-gt-end.rdb"]
craft_slot_import_range_rdb [file join $gen_path empty.rdb] $bad_range_rdb 2000 1000
test {valkey-check-rdb rejects slot-import range with start slot greater than end slot} {
    catch {
        exec $::VALKEY_CHECK_RDB_BIN $bad_range_rdb
    } result
    assert_match {*--- RDB ERROR DETECTED ---*} $result
    assert_match {*Invalid slot import range in RDB: start=2000 end=1000*} $result
    assert_no_match {*RDB looks OK*} $result
}

set bad_range_rdb [file join $range_path "end-out-of-range.rdb"]
craft_slot_import_range_rdb [file join $gen_path empty.rdb] $bad_range_rdb 0 16384
test {valkey-check-rdb rejects slot-import range with end slot out of range} {
    catch {
        exec $::VALKEY_CHECK_RDB_BIN $bad_range_rdb
    } result
    assert_match {*--- RDB ERROR DETECTED ---*} $result
    assert_match {*Invalid slot import range in RDB: start=0 end=16384*} $result
    assert_no_match {*RDB looks OK*} $result
}

set bad_range_rdb [file join $range_path "start-out-of-range.rdb"]
craft_slot_import_range_rdb [file join $gen_path empty.rdb] $bad_range_rdb 16384 17384
test {valkey-check-rdb rejects slot-import range with start slot out of range} {
    catch {
        exec $::VALKEY_CHECK_RDB_BIN $bad_range_rdb
    } result
    assert_match {*--- RDB ERROR DETECTED ---*} $result
    assert_match {*Invalid slot import range in RDB: start=16384 end=17384*} $result
    assert_no_match {*RDB looks OK*} $result
}

set ok_range_rdb [file join $range_path "valid-range.rdb"]
craft_slot_import_range_rdb [file join $gen_path empty.rdb] $ok_range_rdb 0 16383
test {valkey-check-rdb accepts valid slot-import range} {
    catch {
        exec $::VALKEY_CHECK_RDB_BIN $ok_range_rdb
    } result
    assert_match {*RDB looks OK*} $result
    assert_no_match {*RDB ERROR DETECTED*} $result
}

} ;# tags
