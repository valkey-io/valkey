# Sentinel must refuse to start if it can't save state in its config dir.

# Root bypasses file permissions.
if {[exec id -u] != 0} {
    test "Sentinel refuses to start when config dir is not writable" {
        set dir [file normalize "sentinel_ro_dir"]
        file mkdir $dir
        set cfg [file join $dir sentinel.conf]
        set log [file normalize "sentinel_ro_dir.log"]
        set fd [open $cfg w]
        puts $fd "bind 127.0.0.1"
        puts $fd "port [expr {$::sentinel_base_port + 100}]"
        puts $fd "logfile $log"
        close $fd
        file attributes $dir -permissions 0555

        set pid [exec $::VALKEY_SENTINEL_BIN $cfg &]
        wait_for_condition 50 100 {
            ![is_alive $pid]
        } else {
            stop_instance $pid
            file attributes $dir -permissions 0755
            fail "Sentinel started with a read-only config dir"
        }
        file attributes $dir -permissions 0755
        assert_match "*config dir $dir is not writable*" [exec cat $log]
    }
}
