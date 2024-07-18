source tests/support/valkey.tcl
source tests/support/util.tcl

proc bg_server_sleep {host port sec} {
    set r [valkey $host $port 0]
    $r client setname SLEEP_HANDLER
    catch {$r debug sleep $sec}
}

bg_server_sleep [lindex $argv 0] [lindex $argv 1] [lindex $argv 2]
