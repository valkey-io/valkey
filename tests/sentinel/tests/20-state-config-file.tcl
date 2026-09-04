# Verify "sentinel state-config-file": Sentinel persists its runtime state into
# a separate file and never rewrites the (administrator managed) main config
# file. See https://github.com/redis/redis/issues/5226.
#
# This test spawns its own dedicated Sentinel process (independent from the
# shared test fleet) so it can fully control both config files.

proc scf_read_file {path} {
    if {![file exists $path]} { return "" }
    set fd [open $path r]
    set data [read $fd]
    close $fd
    return $data
}

proc scf_wait_up {port} {
    if {[server_is_up 127.0.0.1 $port 100] == 0} {
        fail "state-config-file sentinel did not start on port $port"
    }
}

if {$::tls} {
    puts "Skipping state-config-file test under TLS"
} else {
    set base_dir [file normalize "state-config-file-test"]
    catch {exec rm -rf $base_dir}
    file mkdir $base_dir

    set main_conf [file join $base_dir "sentinel.conf"]
    set state_conf [file join $base_dir "sentinel-state.conf"]
    set port [find_available_port $::sentinel_base_port $::valkey_port_count]

    # Write a main config that references a separate state file.
    set fd [open $main_conf w]
    puts $fd "port $port"
    puts $fd "dir $base_dir"
    puts $fd "logfile log.txt"
    puts $fd "enable-protected-configs yes"
    puts $fd "enable-debug-command yes"
    puts $fd "sentinel state-config-file $state_conf"
    puts $fd "sentinel monitor mymaster 127.0.0.1 12345 2"
    puts $fd "sentinel down-after-milliseconds mymaster 20000"
    close $fd

    set main_conf_orig [scf_read_file $main_conf]

    set pid [exec $::VALKEY_SENTINEL_BIN $main_conf &]
    # Wrap the lifecycle in a catch so that the cleanup below always runs and
    # kills the spawned process, even if a non-assertion error occurs (assertion
    # failures inside "test" are already handled by the test framework).
    catch {
        test "state-config-file: state is written to the separate file, not the main config" {
            scf_wait_up $port
            set link [valkey 127.0.0.1 $port 0 0]
            $link reconnect 1

            # On startup Sentinel picks a myid and flushes state to disk.
            wait_for_condition 50 100 {
                [string match "*sentinel myid*" [scf_read_file $state_conf]]
            } else {
                fail "Sentinel did not create the state config file"
            }
            set state [scf_read_file $state_conf]
            assert_match "*sentinel myid*" $state
            assert_match "*sentinel monitor mymaster*" $state
            assert_match "*sentinel current-epoch*" $state

            # The main config file must be left untouched: no runtime state added.
            assert_equal $main_conf_orig [scf_read_file $main_conf]
        }

        set saved_myid [$link sentinel myid]

        test "state-config-file: SENTINEL SET is persisted to the state file only" {
            $link sentinel set mymaster down-after-milliseconds 12345
            wait_for_condition 50 100 {
                [string match "*down-after-milliseconds mymaster 12345*" [scf_read_file $state_conf]]
            } else {
                fail "SENTINEL SET was not persisted to the state config file"
            }
            # Main config file is still untouched.
            assert_equal $main_conf_orig [scf_read_file $main_conf]
        }

        test "state-config-file: runtime state is restored after restart" {
            catch {$link close}
            exec kill $pid
            wait_for_condition 50 100 {
                [catch {exec kill -0 $pid}] == 1
            } else {
                fail "Sentinel process did not terminate"
            }

            set pid [exec $::VALKEY_SENTINEL_BIN $main_conf &]
            scf_wait_up $port
            set link [valkey 127.0.0.1 $port 0 0]
            $link reconnect 1

            # The Sentinel id is stable across restarts (loaded from the state file).
            assert_equal $saved_myid [$link sentinel myid]

            # The SENTINEL SET override survived the restart.
            set master [$link sentinel master mymaster]
            assert_equal 12345 [dict get $master down-after-milliseconds]
        }
    }

    catch {$link close}
    catch {exec kill $pid}
    catch {exec rm -rf $base_dir}

    # Migration case: the same runtime directives are present in both the main
    # config file (e.g. a pre-split combined config) and the state file. Sentinel
    # must boot without failing on duplicates, with the state file taking
    # precedence.
    set base_dir [file normalize "state-config-file-dup-test"]
    catch {exec rm -rf $base_dir}
    file mkdir $base_dir
    set main_conf [file join $base_dir "sentinel.conf"]
    set state_conf [file join $base_dir "sentinel-state.conf"]
    set port [find_available_port $::sentinel_base_port $::valkey_port_count]

    set fd [open $main_conf w]
    puts $fd "port $port"
    puts $fd "dir $base_dir"
    puts $fd "logfile log.txt"
    puts $fd "sentinel state-config-file $state_conf"
    puts $fd "sentinel monitor mymaster 127.0.0.1 12345 2"
    puts $fd "sentinel known-replica mymaster 127.0.0.1 12346"
    puts $fd "sentinel config-epoch mymaster 3"
    close $fd

    # Pre-existing state file overlapping the main config (different current
    # address and higher epoch).
    set fd [open $state_conf w]
    puts $fd "sentinel myid 0123456789abcdef0123456789abcdef01234567"
    puts $fd "sentinel monitor mymaster 127.0.0.1 12399 2"
    puts $fd "sentinel known-replica mymaster 127.0.0.1 12346"
    puts $fd "sentinel config-epoch mymaster 7"
    puts $fd "sentinel current-epoch 7"
    close $fd

    set pid [exec $::VALKEY_SENTINEL_BIN $main_conf &]

    catch {
        test "state-config-file: duplicate directives across files are tolerated" {
            scf_wait_up $port
            set link [valkey 127.0.0.1 $port 0 0]
            $link reconnect 1

            # myid comes from the state file.
            assert_equal "0123456789abcdef0123456789abcdef01234567" [$link sentinel myid]

            set master [$link sentinel master mymaster]
            # Address and epoch reflect the state file (loaded last / wins).
            assert_equal 12399 [dict get $master port]
            assert_equal 7 [dict get $master config-epoch]
            # The single replica is known exactly once.
            assert_equal 1 [dict get $master num-slaves]
            catch {$link close}
        }
    }

    catch {exec kill $pid}
    catch {exec rm -rf $base_dir}

    # A state file pointing at the main config file is a misconfiguration and
    # Sentinel must refuse to start.
    test "state-config-file: refuses to point at the main config file" {
        set d [file normalize "state-config-file-same-test"]
        catch {exec rm -rf $d}
        file mkdir $d
        set conf [file join $d "sentinel.conf"]
        set p [find_available_port $::sentinel_base_port $::valkey_port_count]
        set fd [open $conf w]
        puts $fd "port $p"
        puts $fd "dir $d"
        puts $fd "logfile log.txt"
        puts $fd "sentinel state-config-file $conf"
        puts $fd "sentinel monitor mymaster 127.0.0.1 12345 2"
        close $fd

        set spid [exec $::VALKEY_SENTINEL_BIN $conf &]
        # The process must refuse to come up.
        assert_equal 0 [server_is_up 127.0.0.1 $p 30]
        wait_for_condition 50 100 {
            [string match "*must differ from the main config file*" [scf_read_file [file join $d "log.txt"]]]
        } else {
            fail "Sentinel did not log the state-config-file conflict error"
        }
        catch {exec kill $spid}
        catch {exec rm -rf $d}
    }
}
