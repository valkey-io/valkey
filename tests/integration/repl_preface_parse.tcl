start_server {tags {"repl"} overrides {save {}}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    $master config set repl-diskless-sync yes
    $master config set repl-diskless-sync-delay 0
    $master config set rdb-compression-mode block
    $master config set rdb-compression-file-mode block

    foreach codec {raw lzf lz4} {
        test "Replica parses framed preface for codec $codec" {
            $master config set rdb-compression-codec $codec
            $master flushall
            for {set i 0} {$i < 16} {incr i} {
                $master set "codec:$codec:$i" [string repeat $codec [expr {$i + 1}]]
            }

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
                    fail "Replica digest mismatch for codec $codec"
                }
            }
        }
    }
}

