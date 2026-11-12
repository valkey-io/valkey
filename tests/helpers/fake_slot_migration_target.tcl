# A fake target node for atomic slot migration tests. It accepts one
# connection from the source, plays the target's side of the protocol up to
# FAILOVER-GRANTED, then sends one extra command, the way a buggy or newer
# target could, and keeps reading until the source closes the link:
#
#   source: CLUSTER SYNCSLOTS ESTABLISH ...   ->  fake: +OK
#   source: <snapshot> CLUSTER SYNCSLOTS SNAPSHOT-EOF
#                                             ->  fake: CLUSTER SYNCSLOTS REQUEST-PAUSE
#   source: CLUSTER SYNCSLOTS PAUSED          ->  fake: CLUSTER SYNCSLOTS REQUEST-FAILOVER
#   source: CLUSTER SYNCSLOTS FAILOVER-GRANTED
#                                             ->  fake: <the extra command>
#
# It prints "SENT" on stdout once the extra command is written, and runs
# until it is killed.
#
# Usage: tclsh fake_slot_migration_target.tcl PORT ARG ?ARG ...?
# (the ARGs are the extra command, for example CLUSTER SYNCSLOTS BOGUS)

set port [lindex $argv 0]
set extra_cmd [lrange $argv 1 end]

proc resp_command {args} {
    set out "*[llength $args]\r\n"
    foreach a $args {
        append out "\$[string length $a]\r\n$a\r\n"
    }
    return $out
}

# Reading line by line is enough to spot the source's messages: each one is
# a RESP array whose last line is the subcommand. The snapshot lines fall
# through unmatched. PING is answered so the test can wait for the fake to
# be up. The fake runs until it is killed.
proc accept {sock host port} {
    fconfigure $sock -translation binary -blocking 0
    fileevent $sock readable [list on_read $sock]
}

proc on_read {sock} {
    global extra_cmd
    while {[gets $sock line] >= 0} {
        set line [string trimright $line "\r"]
        switch -exact -- $line {
            PING {
                puts -nonewline $sock "+PONG\r\n"
            }
            ESTABLISH {
                puts -nonewline $sock "+OK\r\n"
            }
            SNAPSHOT-EOF {
                puts -nonewline $sock [resp_command CLUSTER SYNCSLOTS REQUEST-PAUSE]
            }
            PAUSED {
                puts -nonewline $sock [resp_command CLUSTER SYNCSLOTS REQUEST-FAILOVER]
            }
            FAILOVER-GRANTED {
                puts -nonewline $sock [resp_command {*}$extra_cmd]
                flush $sock
                puts SENT
                flush stdout
            }
        }
        flush $sock
    }
    if {[eof $sock]} {
        close $sock
    }
}

socket -server accept -myaddr 127.0.0.1 $port
vwait forever
