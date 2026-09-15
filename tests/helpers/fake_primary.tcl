# A single-shot fake primary for full-sync negative tests. Answers the
# replication handshake (PING -> +PONG, REPLCONF -> +OK each, PSYNC ->
# +FULLRESYNC), announces a bulk transfer of ANNOUNCE_SIZE bytes, sends the
# contents of PAYLOAD_FILE, then closes the connection and exits.
#
# Usage: tclsh fake_primary.tcl PORT PAYLOAD_FILE ANNOUNCE_SIZE

set port [lindex $argv 0]
set payload_file [lindex $argv 1]
set announce_size [lindex $argv 2]

set fd [open $payload_file r]
fconfigure $fd -translation binary
set payload [read $fd]
close $fd

# The replica sends RESP-encoded commands. Reading line by line and replying
# once per command-name line keeps replies in step with pipelined commands;
# RESP framing lines (*N, $N) and argument lines fall through unmatched.
proc accept {sock host port} {
    global payload announce_size done
    fconfigure $sock -translation binary -blocking 1
    set served 0
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
                puts -nonewline $sock "+FULLRESYNC [string repeat 0 40] 0\r\n"
                puts -nonewline $sock "\$$announce_size\r\n"
                puts -nonewline $sock $payload
                flush $sock
                set served 1
                break
            }
        }
    }
    catch {close $sock}
    # Port-probe connections come and go without a PSYNC; only a served
    # transfer completes the single shot.
    if {$served} {set done served}
}

socket -server accept $port
after 60000 set done timeout
vwait done
