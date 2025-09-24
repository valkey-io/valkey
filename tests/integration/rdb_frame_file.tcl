start_server {tags {"rdb"}} {
    set master [srv 0 client]

    $master config set save ""
    $master config set rdb-compression-mode block
    $master config set rdb-compression-file-mode block

    proc read_frame_header {client} {
        set dir [lindex [$client config get dir] 1]
        set dump_path [file join $dir dump.rdb]
        set fd [open $dump_path r]
        fconfigure $fd -translation binary -encoding binary
        set preamble [read $fd 7]
        set header_line [gets $fd]
        close $fd
        binary scan $preamble c* preamble_bytes
        return [dict create preamble $preamble_bytes header $header_line]
    }

    test "Framed RDB file encodes configured codec and block size" {
        $master config set rdb-compression-codec lzf
        $master config set rdb-compression-block-bytes 131072

        $master flushall
        $master set frame:key value
        assert_equal OK [$master save]

        set header [read_frame_header $master]
        assert_equal {86 75 70 82 77 1 10} [dict get $header preamble]
        assert {[regexp {^codec=lzf blk=131072 checksum=crc64$} [dict get $header header]]}
    }

    test "Framed RDB file header updates after codec changes" {
        $master config set rdb-compression-mode auto
        $master config set rdb-compression-codec raw
        $master config set rdb-compression-block-bytes 1048576

        $master set frame:key2 v2
        assert_equal OK [$master save]

        set header [read_frame_header $master]
        assert_equal {86 75 70 82 77 1 10} [dict get $header preamble]
        assert {[regexp {^codec=raw blk=1048576 checksum=crc64$} [dict get $header header]]}

        $master config set rdb-compression-mode block
    }

    test "Framed RDB file enforces minimum block size" {
        $master config set rdb-compression-codec lz4
        $master config set rdb-compression-block-bytes 4096

        $master set frame:key3 v3
        assert_equal OK [$master save]

        set header [read_frame_header $master]
        assert_equal {86 75 70 82 77 1 10} [dict get $header preamble]
        assert {[regexp {^codec=lz4 blk=65536 checksum=crc64$} [dict get $header header]]}

        $master config set rdb-compression-block-bytes 262144
    }
}
