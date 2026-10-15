proc start_sentinel_instance {extra_configs code} {
    set port [find_available_port 25000 2000]
    set dirname [tmpdir sentinel-[clock milliseconds]-[expr int(rand()*10000)]]
    set cfgfile [file join $dirname "sentinel.conf"]
    set logfile [file join $dirname "sentinel.log"]
    set cfg [open $cfgfile w]
    puts $cfg "port $port"
    puts $cfg "dir $dirname"
    foreach line $extra_configs {
        puts $cfg $line
    }
    close $cfg

    set pid [exec $::VALKEY_SENTINEL_BIN $cfgfile >> $logfile 2>> $logfile &]

    # Wait for sentinel to start
    if {![server_is_up 127.0.0.1 $port 100]} {
        catch {exec kill -9 $pid}
        error "Failed to start sentinel instance on port $port"
    }

    set sc [valkey 127.0.0.1 $port 0 $::tls]
    $sc reconnect 1

    set err ""
    set opt ""
    catch {
        uplevel 1 [list set sentinel_client $sc]
        uplevel 1 $code
    } err opt

    catch {$sc close}
    catch {exec kill -9 $pid}
    if {[dict exists $opt -code] && [dict get $opt -code] != 0} {
        return -options $opt $err
    }
}

test "SENTINEL CONFIG GET aclfile returns empty string when unconfigured" {
    start_sentinel_instance {} {
        set res [$sentinel_client sentinel config get aclfile]
        assert_equal {aclfile {}} $res

        # Wildcard matching
        set res_star [$sentinel_client sentinel config get "acl*"]
        assert_match {*aclfile *} $res_star

        set res_file [$sentinel_client sentinel config get "*file"]
        assert_match {*aclfile *} $res_file

        set res_all [$sentinel_client sentinel config get "*"]
        assert_match {*aclfile *} $res_all

        # SET aclfile must fail
        assert_error "*Invalid argument 'aclfile'*" {
            $sentinel_client sentinel config set aclfile /some/path
        }
    }
}

test "SENTINEL CONFIG GET aclfile returns path when configured" {
    set acl_file [file normalize [tmpfile sentinel-acl.conf]]
    set fp [open $acl_file w]
    puts $fp "user default on nopass ~* &* +@all"
    close $fp

    start_sentinel_instance [list "aclfile $acl_file"] {
        set res [$sentinel_client sentinel config get aclfile]
        assert_equal [list aclfile $acl_file] $res

        # Wildcard matching
        set res_star [$sentinel_client sentinel config get "acl*"]
        assert_match "*aclfile [string map {* \\* ? \\? \\[ \\\[ \\] \\\]} $acl_file]*" $res_star

        set res_file [$sentinel_client sentinel config get "*file"]
        assert_match "*aclfile [string map {* \\* ? \\? \\[ \\\[ \\] \\\]} $acl_file]*" $res_file

        set res_all [$sentinel_client sentinel config get "*"]
        assert_match "*aclfile [string map {* \\* ? \\? \\[ \\\[ \\] \\\]} $acl_file]*" $res_all

        # SET aclfile must still fail
        assert_error "*Invalid argument 'aclfile'*" {
            $sentinel_client sentinel config set aclfile /some/path
        }
    }
}
