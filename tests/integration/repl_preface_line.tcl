start_server {tags {"repl"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    $master config set save ""
    $master config set repl-diskless-sync no
    $master config set rdb-compression-mode block
    $master config set rdb-compression-file-mode block
    $master config set rdb-compression-codec raw

    proc repl_preface_send_command {fd args} {
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

    proc repl_preface_handshake_preface {host port codec_line {blkmax 262144}} {
        set fd [socket $host $port]
        fconfigure $fd -translation binary -encoding binary -buffering none

        repl_preface_send_command $fd {PING}
        set line [gets $fd]
        assert_equal "+PONG" [string trimright $line "\r"]

        repl_preface_send_command $fd {REPLCONF listening-port 0}
        set reply [gets $fd]
        assert_equal "+OK" [string trimright $reply "\r"]

        repl_preface_send_command $fd {REPLCONF rdb-framing yes}
        set reply [gets $fd]
        assert_equal "+OK" [string trimright $reply "\r"]

        repl_preface_send_command $fd [list REPLCONF rdb-codecs $codec_line]
        set reply [gets $fd]
        assert_equal "+OK" [string trimright $reply "\r"]

        repl_preface_send_command $fd [list REPLCONF rdb-blkmax $blkmax]
        set reply [gets $fd]
        assert_equal "+OK" [string trimright $reply "\r"]

        repl_preface_send_command $fd {PSYNC ? -1}
        set fullresync [string trimright [gets $fd] "\r"]
        assert {[regexp {^\+FULLRESYNC} $fullresync]}

        set preface_line [string trimright [gets $fd] "\r"]
        close $fd
        return $preface_line
    }

    test "Replication stream includes framing preface when enabled" {
        set preface_line [repl_preface_handshake_preface $master_host $master_port "raw,lzf,lz4"]
        assert {[regexp {^\+RDBFRAMED codec=(raw|lzf|lz4) blk=[0-9]+ checksum=(crc64|none)$} $preface_line]}
    }

    test "Replication stream omits framing preface when snapshot is legacy" {
        $master config set rdb-compression-mode auto
        $master config set rdb-compression-file-mode legacy

        set fd [socket $master_host $master_port]
        fconfigure $fd -translation binary -encoding binary -buffering none

        repl_preface_send_command $fd {PING}
        set line [gets $fd]
        assert_equal "+PONG" [string trimright $line "\r"]

        repl_preface_send_command $fd {REPLCONF listening-port 0}
        set reply [gets $fd]
        assert_equal "+OK" [string trimright $reply "\r"]

        repl_preface_send_command $fd {REPLCONF rdb-framing yes}
        set reply [gets $fd]
        assert_equal "+OK" [string trimright $reply "\r"]

        repl_preface_send_command $fd {REPLCONF rdb-codecs raw,lzf,lz4}
        set reply [gets $fd]
        assert_equal "+OK" [string trimright $reply "\r"]

        repl_preface_send_command $fd {REPLCONF rdb-blkmax 262144}
        set reply [gets $fd]
        assert_equal "+OK" [string trimright $reply "\r"]

        repl_preface_send_command $fd {PSYNC ? -1}
        set fullresync [string trimright [gets $fd] "\r"]
        assert {[regexp {^\+FULLRESYNC} $fullresync]}

        set first_line [string trimright [gets $fd] "\r"]
        assert {[regexp {^\$[0-9]+$} $first_line]}

        close $fd

        $master config set rdb-compression-mode block
        $master config set rdb-compression-file-mode block
    }

    test "Replication preface uses configured lzf codec when available" {
        $master config set rdb-compression-mode block
        $master config set rdb-compression-file-mode block
        $master config set rdb-compression-codec lzf
        $master config set rdb-compression-checksum crc64

        set preface_line [repl_preface_handshake_preface $master_host $master_port "raw,lzf,lz4"]
        assert {[regexp {^\+RDBFRAMED codec=lzf blk=[0-9]+ checksum=crc64$} $preface_line]}
    }

    test "Replication preface uses configured raw codec when selected" {
        $master config set rdb-compression-mode block
        $master config set rdb-compression-file-mode block
        $master config set rdb-compression-codec raw
        $master config set rdb-compression-checksum crc64

        set preface_line [repl_preface_handshake_preface $master_host $master_port "raw,lzf,lz4"]
        assert {[regexp {^\+RDBFRAMED codec=raw blk=[0-9]+ checksum=crc64$} $preface_line]}
    }

    test "Replication preface uses configured checksum option" {
        $master config set rdb-compression-mode block
        $master config set rdb-compression-file-mode block
        $master config set rdb-compression-codec lz4
        $master config set rdb-compression-checksum none

        set preface_line [repl_preface_handshake_preface $master_host $master_port "raw,lzf,lz4"]
        assert {[regexp {^\+RDBFRAMED codec=lz4 blk=[0-9]+ checksum=none$} $preface_line]}
    }
}
