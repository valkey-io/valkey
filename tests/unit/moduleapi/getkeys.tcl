set testmodule [file normalize tests/modules/getkeys.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {COMMAND INFO correctly reports a movable keys module command} {
        set info [lindex [r command info getkeys.command] 0]

        assert_equal {module movablekeys} [lindex $info 2]
        assert_equal {0} [lindex $info 3]
        assert_equal {0} [lindex $info 4]
        assert_equal {0} [lindex $info 5]
    }

    test {COMMAND GETKEYS correctly reports a movable keys module command} {
        r command getkeys getkeys.command arg1 arg2 key key1 arg3 key key2 key key3
    } {key1 key2 key3}

    test {COMMAND GETKEYS correctly reports a movable keys module command using flags} {
        r command getkeys getkeys.command_with_flags arg1 arg2 key key1 arg3 key key2 key key3
    } {key1 key2 key3}

    test {COMMAND GETKEYSANDFLAGS correctly reports a movable keys module command not using flags} {
        r command getkeysandflags getkeys.command arg1 arg2 key key1 arg3 key key2
    } {{key1 {RW access update}} {key2 {RW access update}}}

    test {COMMAND GETKEYSANDFLAGS correctly reports a movable keys module command using flags} {
        r command getkeysandflags getkeys.command_with_flags arg1 arg2 key key1 arg3 key key2 key key3
    } {{key1 {RO access}} {key2 {RO access}} {key3 {RO access}}}

    test {RM_GetCommandKeys on non-existing command} {
        catch {r getkeys.introspect 0 non-command key1 key2} e
        set _ $e
    } {*ENOENT*}

    test {RM_GetCommandKeys on built-in fixed keys command} {
        r getkeys.introspect 0 set key1 value1
    } {key1}

    test {RM_GetCommandKeys on built-in fixed keys command with flags} {
        r getkeys.introspect 1 set key1 value1
    } {{key1 OW}}

    test {RM_GetCommandKeys with more than 256 keys} {
        # Exceed the inline buffer so leak checkers can catch missing cleanup.
        set keys {}
        for {set i 0} {$i < 300} {incr i} {
            lappend keys "key:$i"
        }

        assert_equal 300 [llength [r getkeys.introspect 0 mget {*}$keys]]
        assert_equal 300 [llength [r getkeys.introspect 1 mget {*}$keys]]
    }

    test {RM_GetCommandKeys on EVAL} {
        r getkeys.introspect 0 eval "" 4 key1 key2 key3 key4 arg1 arg2
    } {key1 key2 key3 key4}

    test {RM_GetCommandKeys on a movable keys module command} {
        r getkeys.introspect 0 getkeys.command arg1 arg2 key key1 arg3 key key2 key key3
    } {key1 key2 key3}

    test {RM_GetCommandKeys on a non-movable module command} {
        r getkeys.introspect 0 getkeys.fixed arg1 key1 key2 key3 arg2
    } {key1 key2 key3}

    test {RM_GetCommandKeys with bad arity} {
        catch {r getkeys.introspect 0 set key} e
        set _ $e
    } {*EINVAL*}

    # user that can only read from "read" keys, write to "write" keys, and read+write to "RW" keys
    r ACL setuser testuser +@all %R~read* %W~write* %RW~rw*

    test "module getkeys-api - ACL" {
        # legacy triple didn't provide flags, so they require both read and write
        assert_equal "OK" [r ACL DRYRUN testuser getkeys.command key rw]
        assert_match {*has no permissions to access the 'read' key*} [r ACL DRYRUN testuser getkeys.command key read]
        assert_match {*has no permissions to access the 'write' key*} [r ACL DRYRUN testuser getkeys.command key write]
    }

    test "module getkeys-api with flags - ACL" {
        assert_equal "OK" [r ACL DRYRUN testuser getkeys.command_with_flags key rw]
        assert_equal "OK" [r ACL DRYRUN testuser getkeys.command_with_flags key read]
        assert_match {*has no permissions to access the 'write' key*} [r ACL DRYRUN testuser getkeys.command_with_flags key write]
    }

    test "Unload the module - getkeys" {
        assert_equal {OK} [r module unload getkeys]
    }
}

tags {tls:skip external:skip cluster} {
set modules [list loadmodule $testmodule]
start_cluster 2 0 [list config_lines $modules overrides {io-threads 2 io-threads-always-active yes cluster-slot-stats-enabled yes}] {
    set local_tag "{06S}"
    set remote_tag "{6ZJ}"

    test "getkeys-api key lookups run on the main thread with I/O threads" {
        set rd [valkey_deferring_client]
        for {set i 0} {$i < 1000} {incr i} {
            $rd getkeys.command key $local_tag$i
        }
        $rd flush
        for {set i 0} {$i < 1000} {incr i} {
            assert_equal [list $local_tag$i] [$rd read]
        }
        $rd close
        assert_morethan [s io_threaded_reads_processed] 0
        assert_equal 0 [R 0 getkeys.off_main_thread_calls]
    }

    test "getkeys-api commands are redirected with I/O threads" {
        assert_equal [list ${local_tag}a] [R 0 getkeys.command key ${local_tag}a]
        assert_error "MOVED 16383 *" {R 0 getkeys.command key ${remote_tag}a}
        assert_error "CROSSSLOT *" {R 0 getkeys.command key ${local_tag}a key ${remote_tag}a}
    }

    test "CLUSTER SLOT-STATS counts ACL-rejected getkeys-api commands with I/O threads" {
        R 0 ACL SETUSER limited on nopass +@all -getkeys.command
        set c [valkey_client]
        $c AUTH limited pass
        set slot [R 0 CLUSTER KEYSLOT ${local_tag}k]

        # *3\r\n$15\r\ngetkeys.command\r\n$3\r\nkey\r\n$6\r\n{06S}k\r\n --> 47 bytes.
        R 0 CONFIG RESETSTAT
        assert_error {*NOPERM*} {$c getkeys.command key ${local_tag}k}
        set stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE $slot $slot]
        assert_equal 47 [dict get [lindex $stats 0 1] network-bytes-in]

        $c close
        R 0 ACL DELUSER limited
    }
}
}
