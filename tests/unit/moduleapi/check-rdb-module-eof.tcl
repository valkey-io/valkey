# The RDB checker must report a module value that is cut off at an opcode
# boundary (before its EOF marker) instead of looping forever.
set testmodule [file normalize tests/modules/datatype.so]

# Run a command in the background with its output in $out. Wait up to
# $seconds for it to exit; if it is still running, kill it and return 0.
proc run_with_deadline {seconds out args} {
    set pid [exec {*}$args >& $out &]
    for {set i 0} {$i < $seconds * 10} {incr i} {
        if {![process_is_alive $pid]} { return 1 }
        after 100
    }
    catch {exec kill -9 $pid}
    return 0
}

tags "modules external:skip" {
    set server_path [file normalize [tmpdir "server.check-rdb-module-eof"]]
    start_server [list overrides [list loadmodule "$testmodule" dir $server_path] keep_persistence true] {
        r datatype.set k 1 truncatemarker
        r save
    }

    # Cut the file right after the last field of the module value, so the
    # next read is the module value's EOF opcode and hits the end of file.
    set fd [open $server_path/dump.rdb rb]
    set data [read $fd]
    close $fd
    set end [expr {[string first truncatemarker $data] + [string length truncatemarker]}]
    assert {$end > [string length truncatemarker]}
    set fd [open $server_path/truncated.rdb wb]
    puts -nonewline $fd [string range $data 0 [expr {$end - 1}]]
    close $fd

    test {valkey-check-rdb reports an RDB truncated inside a module value} {
        set exited [run_with_deadline 10 $server_path/check.out \
            $::VALKEY_CHECK_RDB_BIN $server_path/truncated.rdb]
        set output [exec cat $server_path/check.out]
        assert_equal $exited 1 "valkey-check-rdb still running after 10s; output: $output"
        assert_match {*RDB ERROR DETECTED*} $output
    }

    test {Startup load of an RDB truncated inside a module value exits} {
        set port [find_available_port $::baseport $::portcount]
        set exited [run_with_deadline 10 $server_path/startup.out \
            $::VALKEY_SERVER_BIN --port $port --dir $server_path --dbfilename truncated.rdb \
            --save "" --loadmodule $testmodule --logfile $server_path/startup.log]
        set output [exec cat $server_path/startup.out]
        assert_equal $exited 1 "valkey-server still running after 10s; checker output: $output"
        assert_match {*Unexpected EOF reading RDB file*} [exec cat $server_path/startup.log]
        assert_match {*RDB ERROR DETECTED*} $output
    }
}
