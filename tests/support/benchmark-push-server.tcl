# Minimal RESP server for checking that benchmark ignores unsolicited pushes.
set marker [lindex $argv 0]
proc accept {channel address port} {
    global marker
    fconfigure $channel -translation binary -buffering none
    while {[gets $channel header] >= 0} {
        set header [string trimright $header "\r"]
        if {[string index $header 0] ne "*"} { error "bad request" }
        set count [string range $header 1 end]
        set args {}
        for {set i 0} {$i < $count} {incr i} {
            gets $channel len
            set len [string range [string trimright $len "\r"] 1 end]
            lappend args [read $channel $len]
            read $channel 2
        }
        set command [string toupper [lindex $args 0]]
        if {$command eq "CLIENT"} {
            puts -nonewline $channel "+OK\r\n"
        } elseif {$command eq "HELLO"} {
            puts -nonewline $channel "%0\r\n"
        } elseif {$command eq "PING"} {
            puts -nonewline $channel ">2\r\n\$10\r\ninvalidate\r\n*1\r\n\$3\r\nkey\r\n"
            after 500
            close [open $marker w]
            catch {puts -nonewline $channel "+PONG\r\n"}
            close $channel
            exit 0
        } else {
            error "unexpected command $command"
        }
    }
    exit 1
}
set server [socket -server accept -myaddr 127.0.0.1 0]
puts [lindex [fconfigure $server -sockname] 2]
flush stdout
vwait forever
