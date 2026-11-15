# Copyright (c) Valkey Contributors
# All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause

# Regenerate the Redis 8.6 stream fixtures using a disposable, empty Redis 8.6.0
# server, not a production instance:
# tclsh tests/helpers/gen-redis-streams.tcl HOST PORT OUTPUT_DIRECTORY
# The server must allow CONFIG SET and SAVE. No Redis executable is needed to
# run the regression tests; only the generated data files are checked in.
source tests/support/valkey.tcl

if {[llength $argv] != 3} {
    error "Usage: gen-redis-streams.tcl HOST PORT OUTPUT_DIRECTORY"
}
lassign $argv host port output
set r [valkey $host $port]
if {![string match "*redis_version:8.6.0\r\n*" [$r info server]]} {
    error "Fixture generation requires Redis 8.6.0"
}
if {[$r dbsize] != 0} {
    error "Fixture generation requires an empty database"
}
$r config set stream-node-max-entries 2

for {set i 1} {$i <= 6} {incr i} {
    $r xadd plain $i-0 field $i
}
$r xadd plain 7-1 other "binary\x00\xff"
$r xdel plain 3-0
$r xgroup create plain first 0
$r xgroup create plain second 0
$r xreadgroup group first alice count 3 streams plain >
$r xreadgroup group first bob count 1 streams plain >
$r xack plain first 1-0
$r xclaim plain first bob 0 2-0 time 123456789 retrycount 7 justid
$r xdel plain 2-0
$r xgroup createconsumer plain second inactive

$r xadd emptied 1-0 f v
$r xgroup create emptied group 0
$r xreadgroup group group consumer streams emptied >
$r xdel emptied 1-0
$r xsetid emptied 5-9 entriesadded 8 maxdeletedid 3-0

$r xgroup create no-entries group 0 mkstream
$r xgroup createconsumer no-entries group inactive

$r xadd idmp idmp 123 42 * f first
$r xadd idmp idmp 123 43 * f second
$r xadd idmp idmp 123 42 * f duplicate
$r xadd idmp idmp [string repeat producer 30] [string repeat iid 60] * f third
$r set sentinel after-streams

set expected {}
set dumps {}
foreach key {plain emptied no-entries idmp} {
    dict set expected $key [$r xinfo stream $key full count 0]
    binary scan [$r dump $key] H* dump
    dict set dumps $key $dump
}
$r save
set dir [lindex [$r config get dir] 1]
set filename [lindex [$r config get dbfilename] 1]
file copy -force [file join $dir $filename] [file join $output redis-8.6-streams.rdb]
set fd [open [file join $output redis-8.6-streams.txt] w]
# Hex keeps binary stream values independent of the file's text encoding.
binary scan [list $expected $dumps] H* manifest
puts $fd $manifest
close $fd
$r close
