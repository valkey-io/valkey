# A fake primary for testing errors while a dual-channel replica replays its
# buffered command stream. The main channel negotiates dual-channel sync; the
# RDB channel receives a valid size-framed RDB, while the main channel receives
# the supplied command-stream bytes before the RDB finishes loading.
#
# Usage: tclsh fake_dual_channel_primary.tcl PORT RDB_FILE STREAM_FILE

set port [lindex $argv 0]
set rdb_file [lindex $argv 1]
set stream_file [lindex $argv 2]

set fd [open $rdb_file r]
fconfigure $fd -translation binary
set rdb_payload [read $fd]
close $fd

set fd [open $stream_file r]
fconfigure $fd -translation binary
set stream_payload [read $fd]
close $fd

array set channel {}
set main_psync_count 0
set stream_sent 0
set done 0

proc send_bytes {sock bytes} {
    puts -nonewline $sock $bytes
    flush $sock
}

proc send_rdb {sock} {
    global rdb_payload
    catch {
        send_bytes $sock "\$[string length $rdb_payload]\r\n$rdb_payload"
    }
    catch {close $sock}
}

proc read_command {sock} {
    global channel main_psync_count stream_payload stream_sent server_socket done

    while {[gets $sock line] >= 0} {
        set command [string toupper [string trim $line]]
        if {$command eq "PING"} {
            set channel($sock) main
            send_bytes $sock "+PONG\r\n"
        } elseif {$command eq "REPLCONF"} {
            send_bytes $sock "+OK\r\n"
        } elseif {$command eq "PSYNC"} {
            set channel($sock) main
            incr main_psync_count
            if {$main_psync_count == 1} {
                send_bytes $sock "+DUALCHANNELSYNC\r\n"
            } else {
                send_bytes $sock "+CONTINUE [string repeat 0 40]\r\n$stream_payload"
                set stream_sent 1
                catch {close $server_socket}
            }
        } elseif {$command eq "SYNC"} {
            set channel($sock) rdb
            send_bytes $sock "\$ENDOFF:0 [string repeat 0 40] 0 1\r\n"
            after 50 [list send_rdb $sock]
        }
    }

    if {[eof $sock]} {
        set was_main [expr {[info exists channel($sock)] && $channel($sock) eq "main"}]
        catch {close $sock}
        catch {unset channel($sock)}
        if {$was_main && $stream_sent} {
            set done served
        }
    }
}

proc accept {sock host port} {
    global channel
    fconfigure $sock -translation binary -blocking 0 -buffering none
    set channel($sock) unknown
    fileevent $sock readable [list read_command $sock]
}

set server_socket [socket -server accept $port]
after 60000 set done timeout
vwait done
