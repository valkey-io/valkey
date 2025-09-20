proc repl_matrix_send_command {fd args} {
    if {[llength $args] == 1} {
        set args [lindex $args 0]
    }

    set cmd "*[llength $args]\r\n"
    foreach arg $args {
        append cmd "$" [string length $arg] "\r\n" $arg "\r\n"
    }
    puts -nonewline $fd $cmd
    flush $fd
}

start_server {tags {"repl"} overrides {save {}}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    $master config set repl-diskless-sync yes
    $master config set repl-diskless-sync-delay 0
    $master config set rdb-compression-mode block
    $master config set rdb-compression-file-mode block
    $master config set rdb-compression-codec raw

    test "New master with old replica uses legacy stream" {
        set fd [socket $master_host $master_port]
        fconfigure $fd -translation binary -encoding binary -buffering none

        repl_matrix_send_command $fd {PING}
        gets $fd

        repl_matrix_send_command $fd {REPLCONF listening-port 0}
        gets $fd

        repl_matrix_send_command $fd {REPLCONF capa eof capa psync2}
        gets $fd

        repl_matrix_send_command $fd {REPLCONF version 0.0.0}
        gets $fd

        repl_matrix_send_command $fd {PSYNC ? -1}
        set fullresync [string trimright [gets $fd] "\r"]
        assert {[regexp {^\+FULLRESYNC} $fullresync]}

        set first_line [string trimright [gets $fd] "\r"]
        set legacy_header [regexp {^\$[0-9]+$} $first_line]
        set eof_header [regexp {^\$EOF:} $first_line]
        assert {[expr {$legacy_header || $eof_header}]}

        close $fd
    }
}

start_server {tags {"repl"} overrides {save {}}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    $master config set repl-diskless-sync yes
    $master config set repl-diskless-sync-delay 0
    $master config set rdb-compression-mode legacy
    $master config set rdb-compression-file-mode legacy

    $master flushall
    for {set i 0} {$i < 8} {incr i} {
        $master set "legacy:$i" [string repeat "x" [expr {$i + 1}]]
    }

    test "Old master with new replica loads legacy stream" {
        start_server {overrides {save {}}} {
            set replica [srv 0 client]
            $replica config set repl-diskless-load swapdb

            $replica replicaof $master_host $master_port

            wait_for_condition 100 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replica failed to synchronize"
            }

            wait_for_condition 100 100 {
                [$replica debug digest] eq [$master debug digest]
            } else {
                fail "Replica digest mismatch for legacy stream"
            }
        }
    }
}

start_server {tags {"repl"} overrides {save {}}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    $master config set repl-diskless-sync yes
    $master config set repl-diskless-sync-delay 0
    $master config set rdb-compression-mode block
    $master config set rdb-compression-file-mode block
    $master config set rdb-compression-codec lz4

    $master flushall
    for {set i 0} {$i < 12} {incr i} {
        $master set "framed:$i" [string repeat "f" [expr {$i + 5}]]
    }

    test "New master with new replica exchanges framed stream" {
        start_server {overrides {save {}}} {
            set replica [srv 0 client]
            $replica config set repl-diskless-load swapdb

            $replica replicaof $master_host $master_port

            wait_for_condition 100 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replica failed to synchronize"
            }

            wait_for_condition 100 100 {
                [$replica debug digest] eq [$master debug digest]
            } else {
                fail "Replica digest mismatch for framed stream"
            }
        }
    }
}

