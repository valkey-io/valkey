# A fake node for replaying predefined/expected traffic with a client.
#
# Usage: tclsh fake_redis_node.tcl PORT COMMAND REPLY [ COMMAND REPLY [ ... ] ]
#
# Commands are given as space-separated strings, e.g. "GET foo", and replies as
# RESP-encoded replies minus the trailing \r\n, e.g. "+OK". Prefix a reply
# with "hex:" to send decoded binary content. Dynamic PSYNC expectations may
# opt into glob matching with an explicit "glob:" prefix.

set port [lindex $argv 0];
set expected_traffic [lrange $argv 1 end];

# Reads and parses a command from a socket and returns it as a space-separated
# string, e.g. "set foo bar".
proc read_command {sock} {
    set char [read $sock 1]
    switch $char {
        * {
            set numargs [gets $sock]
            set result {}
            for {set i 0} {$i<$numargs} {incr i} {
                read $sock 1;       # dollar sign
                set len [gets $sock]
                set str [read $sock $len]
                gets $sock;         # trailing \r\n
                lappend result $str
            }
            return $result
        }
        {} {
            # EOF
            return {}
        }
        default {
            # Non-RESP command
            set rest [gets $sock]
            return "$char$rest"
        }
    }
}

proc accept {sock host port} {
    global expected_traffic
    # Preserve binary reply bytes while retaining the default CRLF translation.
    fconfigure $sock -encoding iso8859-1
    foreach {expect_cmd reply} $expected_traffic {
        if {[eof $sock]} {break}
        set cmd [read_command $sock]
        set command_matches [string equal -nocase $cmd $expect_cmd]
        if {!$command_matches &&
            [string equal -nocase [string range $expect_cmd 0 10] "glob:PSYNC "] &&
            [string equal -nocase [lindex $cmd 0] "PSYNC"]} {
            set command_matches [string match -nocase [string range $expect_cmd 5 end] $cmd]
        }
        if {$command_matches} {
            if {[string equal [string range $reply 0 3] "hex:"]} {
                set reply [binary format H* [string range $reply 4 end]]
            }
            puts $sock $reply
            flush $sock
        } else {
            puts $sock "-ERR unexpected command $cmd"
            break
        }
    }
    close $sock
}

socket -server accept $port
after 5000 set done timeout
vwait done
