tags {"rdb-compression external:skip needs:debug"} {

proc read_binary_file_prefix {path count} {
    set fd [open $path r]
    fconfigure $fd -translation binary
    set prefix [read $fd $count]
    close $fd
    return $prefix
}

proc dump_rdb_path {client} {
    return [file join [lindex [$client config get dir] 1] dump.rdb]
}

proc read_dump_rdb_header_bytes {client} {
    return [read_binary_file_prefix [dump_rdb_path $client] 8]
}

proc write_rdb_test_dataset {client prefix} {
    $client flushall
    for {set i 0} {$i < 12} {incr i} {
        $client set "${prefix}:str:$i" [string repeat "${prefix}:value:$i " 16]
    }
    $client lpush "${prefix}:list" a b c d e
    $client sadd "${prefix}:set" alpha beta gamma
    $client zadd "${prefix}:zset" 1 one 2 two 3 three
    $client hset "${prefix}:hash" f1 v1 f2 [string repeat "${prefix}:hash " 8]
    $client xadd "${prefix}:stream" * f1 s1 f2 [string repeat "${prefix}:stream " 4]
    $client xadd "${prefix}:stream" * f1 s2 f2 tail
}

proc assert_rdb_test_dataset {client prefix} {
    assert_equal [string repeat "${prefix}:value:0 " 16] [$client get "${prefix}:str:0"]
    assert_equal 17 [$client dbsize]
    assert_equal 5 [$client llen "${prefix}:list"]
    assert_equal 3 [$client scard "${prefix}:set"]
    assert_equal 3 [$client zcard "${prefix}:zset"]
    assert_equal v1 [$client hget "${prefix}:hash" f1]
    assert_equal 2 [$client xlen "${prefix}:stream"]
}

start_server {overrides {save "" enable-debug-command local}} {
    test {RDB save and load round-trip with LZ4 compression} {
        set prefix "lz4-round-trip"
        r config set rdbcompression yes
        r config set rdb-compression-algo lz4
        write_rdb_test_dataset r $prefix
        assert_rdb_test_dataset r $prefix
        set digest [debug_digest]

        assert_equal "OK" [r save]
        r config rewrite
        restart_server 0 true false

        assert_equal "lz4" [lindex [r config get rdb-compression-algo] 1]
        set newdigest [debug_digest]
        assert {$digest eq $newdigest}
        assert_rdb_test_dataset r $prefix
    }

    test {Empty LZ4-compressed RDB saves and loads correctly} {
        r config set rdbcompression yes
        r config set rdb-compression-algo lz4
        r flushall

        assert_equal 0 [r dbsize]
        assert_equal "OK" [r save]
        r config rewrite
        assert_equal "VKCS" [string range [read_dump_rdb_header_bytes r] 0 3]

        restart_server 0 true false

        assert_equal "lz4" [lindex [r config get rdb-compression-algo] 1]
        assert_equal 0 [r dbsize]
    }

    test {RDB save with LZF (default) round-trips correctly} {
        set prefix "lzf-round-trip"
        r config set rdbcompression yes
        r config set rdb-compression-algo lzf
        write_rdb_test_dataset r $prefix
        assert_rdb_test_dataset r $prefix

        set digest [debug_digest]
        assert_equal "OK" [r save]
        r config rewrite
        restart_server 0 true false

        assert_equal "lzf" [lindex [r config get rdb-compression-algo] 1]
        set newdigest [debug_digest]
        assert {$digest eq $newdigest}
        assert_rdb_test_dataset r $prefix
    }

    test {Uncompressed RDB files load correctly (backward compat)} {
        set prefix "plain-rdb"
        r config set rdbcompression no
        r config set rdb-compression-algo lzf
        write_rdb_test_dataset r $prefix
        assert_rdb_test_dataset r $prefix

        set digest [debug_digest]
        assert_equal "OK" [r save]
        r config rewrite
        restart_server 0 true false

        assert_equal "no" [lindex [r config get rdbcompression] 1]
        set newdigest [debug_digest]
        assert {$digest eq $newdigest}
        assert_rdb_test_dataset r $prefix
    }

    test {LZ4 compressed RDB with large dataset} {
        r config set rdbcompression yes
        r config set rdb-compression-algo lz4
        r flushall
        for {set i 0} {$i < 1000} {incr i} {
            r set "bulk:$i" [string repeat "payload:$i " 32]
        }
        r lpush bulk:list a b c d e
        r hset bulk:hash f1 v1 f2 v2

        set digest [debug_digest]
        assert_equal "OK" [r save]
        r config rewrite
        restart_server 0 true false

        assert_equal "lz4" [lindex [r config get rdb-compression-algo] 1]
        set newdigest [debug_digest]
        assert {$digest eq $newdigest}
        assert_equal [string repeat "payload:42 " 32] [r get bulk:42]
        assert_equal 5 [r llen bulk:list]
        assert_equal "v1" [r hget bulk:hash f1]
        assert_equal 1002 [r dbsize]
    }

    test {Changing compression config during active BGSAVE does not affect the in-flight save} {
        r config set rdbcompression yes
        r config set rdb-compression-algo lz4
        r config set rdb-key-save-delay 10000
        r flushall
        for {set i 0} {$i < 128} {incr i} {
            r set "bgsave-race:$i" [string repeat "payload:$i " 128]
        }

        assert_match {*Background saving started*} [r bgsave]
        wait_for_condition 200 10 {
            [s rdb_bgsave_in_progress] eq 1
        } else {
            r config set rdb-key-save-delay 0
            fail "BGSAVE did not start in time"
        }

        r config set rdb-compression-algo lzf

        wait_for_condition 500 10 {
            [s rdb_bgsave_in_progress] eq 0
        } else {
            r config set rdb-key-save-delay 0
            fail "BGSAVE did not finish in time"
        }
        r config set rdb-key-save-delay 0

        assert_equal "lzf" [lindex [r config get rdb-compression-algo] 1]
        assert_equal "VKCS" [string range [read_dump_rdb_header_bytes r] 0 3]

        assert_equal "OK" [r save]
        assert_equal "VALKEY" [string range [read_dump_rdb_header_bytes r] 0 5]

        r config set rdb-compression-algo lz4
    }

    test {Switching from LZ4 to LZF preserves data} {
        set prefix "lz4-to-lzf"
        r config set rdbcompression yes
        r config set rdb-compression-algo lz4
        write_rdb_test_dataset r $prefix
        assert_rdb_test_dataset r $prefix
        set digest [debug_digest]

        # Save with LZ4, then restart with LZF and load the existing file.
        assert_equal "OK" [r save]
        r config set rdb-compression-algo lzf
        r config rewrite
        restart_server 0 true false

        assert_equal "lzf" [lindex [r config get rdb-compression-algo] 1]
        set newdigest [debug_digest]
        assert {$digest eq $newdigest}
        assert_rdb_test_dataset r $prefix
    }

    test {Switching from LZF to LZ4 preserves data} {
        set prefix "lzf-to-lz4"
        r config set rdbcompression yes
        r config set rdb-compression-algo lzf
        write_rdb_test_dataset r $prefix
        assert_rdb_test_dataset r $prefix
        set digest [debug_digest]

        # Save with LZF, then restart with LZ4 and load the existing file.
        assert_equal "OK" [r save]
        r config set rdb-compression-algo lz4
        r config rewrite
        restart_server 0 true false

        assert_equal "lz4" [lindex [r config get rdb-compression-algo] 1]
        set newdigest [debug_digest]
        assert {$digest eq $newdigest}
        assert_rdb_test_dataset r $prefix
    }

    test {Invalid compression algo config is rejected} {
        catch {r config set rdb-compression-algo snappy} err
        assert_match "*argument(s) must be one of the following: lzf, lz4*" $err
    }

    test {RDB compression configs survive CONFIG REWRITE and restart} {
        r config set rdbcompression yes
        r config set rdb-compression-algo lz4
        r config rewrite

        restart_server 0 true false

        assert_equal "yes" [lindex [r config get rdbcompression] 1]
        assert_equal "lz4" [lindex [r config get rdb-compression-algo] 1]
    }

    test {LZ4 compressed RDB with rdbchecksum yes sets the VKCS codec checksum flag} {
        r config set rdbcompression yes
        r config set rdb-compression-algo lz4
        r flushall
        for {set i 0} {$i < 200} {incr i} {
            r set "cksum:$i" [string repeat "payload$i " 200]
        }

        r save
        set header [read_dump_rdb_header_bytes r]
        assert_equal "VKCS" [string range $header 0 3]
        binary scan [string index $header 6] cu flags
        assert {$flags == 1}

        set digest [debug_digest]
        restart_server 0 true false
        set newdigest [debug_digest]
        assert {$digest eq $newdigest}
        assert_equal [string repeat "payload42 " 200] [r get cksum:42]
    }

    test {Partial VKCS snapshot copied from an interrupted BGSAVE is rejected on load} {
        r config set rdbcompression yes
        r config set rdb-compression-algo lz4
        r config set rdb-key-save-delay 10000
        r flushall
        set noisy_payload ""
        for {set j 0} {$j < 32768} {incr j} {
            append noisy_payload [format %c [expr {(($j * 31) + 17) % 94 + 33}]]
        }
        for {set i 0} {$i < 128} {incr i} {
            r set "partial:$i" "${noisy_payload}:$i"
        }

        assert_match {*Background saving started*} [r bgsave]
        wait_for_condition 200 10 {
            [s rdb_bgsave_in_progress] eq 1
        } else {
            r config set rdb-key-save-delay 0
            fail "BGSAVE did not start in time"
        }

        wait_for_condition 200 10 {
            [get_child_pid 0] ne ""
        } else {
            r config set rdb-key-save-delay 0
            fail "Timed out waiting for BGSAVE child pid"
        }
        set child_pid [get_child_pid 0]
        set dir [lindex [r config get dir] 1]
        set temp_rdb [file join $dir temp-${child_pid}.rdb]
        set partial_rdb [file join $dir partial-vkcs.rdb]
        wait_for_condition 500 10 {
            [file exists $temp_rdb] && [file size $temp_rdb] > 4096
        } else {
            r config set rdb-key-save-delay 0
            catch {exec kill -9 $child_pid}
            fail "Timed out waiting for partial VKCS snapshot"
        }

        file copy -force $temp_rdb $partial_rdb
        assert_equal "VKCS" [string range [read_binary_file_prefix $partial_rdb 8] 0 3]

        catch {exec kill -9 $child_pid}
        wait_for_condition 500 10 {
            [s rdb_bgsave_in_progress] eq 0
        } else {
            r config set rdb-key-save-delay 0
            fail "Interrupted BGSAVE child was not collected in time"
        }
        r config set rdb-key-save-delay 0

        file copy -force $partial_rdb [dump_rdb_path r]
        catch {r debug reload nosave} err
        assert_match "*Error*" $err
    }

    test {LZ4 compressed RDB detects tail corruption when codec checksums are enabled} {
        r config set rdb-compression-algo lz4
        r flushall
        for {set i 0} {$i < 100} {incr i} {
            r set "footer:$i" [string repeat "payload$i " 100]
        }

        r save
        set rdbfile [file join [lindex [r config get dir] 1] dump.rdb]
        set fd [open $rdbfile r+]
        fconfigure $fd -translation binary
        seek $fd -8 end
        puts -nonewline $fd "foobar00"
        close $fd

        catch {r debug reload nosave} err
        assert_match "*Error*" $err
    }

    test {LZ4 compressed RDB with REPL stream kind is rejected} {
        r config set rdbcompression yes
        r config set rdb-compression-algo lz4
        r flushall
        r set wrong-kind:key [string repeat "payload " 100]

        assert_equal "OK" [r save]

        set rdbfile [file join [lindex [r config get dir] 1] dump.rdb]
        set fd [open $rdbfile r+]
        fconfigure $fd -translation binary
        set data [read $fd]
        set data [string replace $data 7 7 [binary format c 0x01]]
        seek $fd 0
        puts -nonewline $fd $data
        close $fd

        catch {r debug reload nosave} err
        assert_match "*Error*" $err
    }

    test {LZ4 compressed RDB detects corruption in compressed payload} {
        r config set rdbcompression yes
        r config set rdb-compression-algo lz4
        r flushall
        for {set i 0} {$i < 100} {incr i} {
            r set "corrupt:$i" [string repeat "testdata$i " 100]
        }

        assert_equal "OK" [r save]

        set rdbfile [file join [lindex [r config get dir] 1] dump.rdb]

        # Read the file, flip a byte in the compressed payload
        # (skip the VKCS envelope at offset 0-7, corrupt somewhere in the middle)
        set fd [open $rdbfile r+]
        fconfigure $fd -translation binary
        set data [read $fd]
        set len [string length $data]
        # Corrupt a byte roughly in the middle of the compressed data
        set pos [expr {$len / 2}]
        set byte [string index $data $pos]
        binary scan $byte c val
        set newval [expr {($val + 1) & 0xFF}]
        set newbyte [binary format c $newval]
        set data [string replace $data $pos $pos $newbyte]
        seek $fd 0
        puts -nonewline $fd $data
        close $fd

        # Reload should fail due to corruption
        catch {r debug reload nosave} err
        assert_match "*Error*" $err
    }

    test {Invalid non-VKCS/non-RDB file fails reload} {
        r config set rdbcompression yes
        r config set rdb-compression-algo lz4
        r flushall
        r set smoke-key smoke-value

        assert_equal "OK" [r save]

        set rdbfile [file join [lindex [r config get dir] 1] dump.rdb]
        set fd [open $rdbfile w]
        fconfigure $fd -translation binary
        puts -nonewline $fd "NOTANRDB"
        close $fd

        catch {r debug reload nosave} err
        assert_match "*Error*" $err
    }
}

start_server {config "minimal.conf" args {"--rdb-compression-algo lz4"}} {
    test {Startup accepts valid LZ4 compression config} {
        assert_equal "lz4" [lindex [r config get rdb-compression-algo] 1]
    }
}

start_server {overrides {save "" enable-debug-command local rdbchecksum no}} {
    test {LZ4 compressed RDB with rdbchecksum no leaves VKCS flags clear and loads correctly} {
        r config set rdbcompression yes
        r config set rdb-compression-algo lz4
        r flushall
        for {set i 0} {$i < 50} {incr i} {
            r set "nocksum:$i" [string repeat "data$i " 100]
        }

        r save
        set header [read_dump_rdb_header_bytes r]
        assert_equal "VKCS" [string range $header 0 3]
        binary scan [string index $header 6] cu flags
        assert {$flags == 0}

        set digest [debug_digest]
        restart_server 0 true false
        set newdigest [debug_digest]
        assert {$digest eq $newdigest}
        assert_equal [string repeat "data10 " 100] [r get nocksum:10]
    }
}

start_server {tags {"rdb-compression repl external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]

    start_server {overrides {save "" enable-debug-command local}} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        test {Disk-based full sync replication works with LZ4-compressed RDB snapshot} {
            $primary config set rdbcompression yes
            $primary config set rdb-compression-algo lz4
            $primary flushall
            for {set i 0} {$i < 500} {incr i} {
                $primary set "repl:$i" [string repeat "payload$i " 40]
            }

            $replica replicaof $primary_host $primary_port
            wait_for_sync $replica

            wait_for_condition 50 100 {
                [status $replica master_link_status] eq "up" &&
                [$primary debug digest] eq [$replica debug digest]
            } else {
                fail "Replica digest mismatch after LZ4 RDB full sync"
            }

            assert_equal [string repeat "payload42 " 40] [$replica get repl:42]
        }

        test {Incremental replication continues after LZ4 full sync} {
            $primary set repl:post-sync "after-sync"
            wait_for_condition 50 100 {
                [$replica get repl:post-sync] eq "after-sync"
            } else {
                fail "Replica did not receive post-sync write"
            }
        }

        test {Diskless full sync remains compatible when rdb-compression-algo is lz4} {
            $replica replicaof no one
            $primary config set repl-diskless-sync yes
            $primary config set repl-diskless-sync-delay 0
            $replica config set repl-diskless-load swapdb
            $primary flushall
            for {set i 0} {$i < 300} {incr i} {
                $primary set "diskless:$i" [string repeat "payload$i " 60]
            }

            $replica replicaof $primary_host $primary_port
            wait_for_sync $replica

            wait_for_condition 50 100 {
                [status $replica master_link_status] eq "up" &&
                [$primary debug digest] eq [$replica debug digest]
            } else {
                fail "Replica digest mismatch after diskless LZ4 full sync"
            }

            assert_equal [string repeat "payload42 " 60] [$replica get diskless:42]
            $primary config set repl-diskless-sync no
        }

        $replica replicaof no one
    }
}

set cluster_bus_port [find_available_port $::baseport $::portcount]
start_server [list tags {"rdb-compression cluster external:skip singledb"} overrides [list save "" cluster-enabled yes cluster-port $cluster_bus_port]] {
    test {RDB compression config works in cluster mode} {
        r config set rdb-compression-algo lz4
        assert_equal "OK" [r save]
    }
}

}
