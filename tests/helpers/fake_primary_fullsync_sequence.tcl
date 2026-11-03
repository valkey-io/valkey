# A fake primary that serves a sequence of full-sync outcomes. F sends a
# truncated RDB, S sends a valid RDB then closes, and H sends a valid RDB and
# keeps the connection open. Each PSYNC attempt timestamp is appended to
# COUNT_FILE.
#
# Usage: tclsh fake_primary_fullsync_sequence.tcl PORT RDB_FILE COUNT_FILE F,S,F,H

set port [lindex $argv 0]
set rdb_file [lindex $argv 1]
set count_file [lindex $argv 2]
set outcomes [split [lindex $argv 3] ,]
set attempts 0

set fd [open $rdb_file r]
fconfigure $fd -translation binary
set rdb_payload [read $fd]
close $fd

proc record_attempt {} {
    global count_file
    set fd [open $count_file a]
    puts $fd [clock milliseconds]
    close $fd
}

proc accept {sock host port} {
    global attempts outcomes rdb_payload

    fconfigure $sock -translation binary -blocking 1
    catch {
        while {[gets $sock line] >= 0} {
            set cmd [string toupper [string trim $line]]
            if {$cmd eq "PING"} {
                puts -nonewline $sock "+PONG\r\n"
                flush $sock
            } elseif {$cmd eq "REPLCONF"} {
                puts -nonewline $sock "+OK\r\n"
                flush $sock
            } elseif {$cmd eq "PSYNC"} {
                record_attempt
                set outcome [lindex $outcomes $attempts]
                incr attempts
                if {$outcome eq ""} {set outcome H}

                puts -nonewline $sock "+FULLRESYNC [string repeat 0 40] 0\r\n"
                if {$outcome eq "F"} {
                    puts -nonewline $sock "\$1\r\n"
                    flush $sock
                    break
                }

                puts -nonewline $sock "\$[string length $rdb_payload]\r\n$rdb_payload"
                flush $sock
                if {$outcome eq "H"} {
                    while {![eof $sock]} {
                        read $sock 4096
                    }
                }
                break
            }
        }
    }
    catch {close $sock}
}

socket -server accept $port
vwait forever
