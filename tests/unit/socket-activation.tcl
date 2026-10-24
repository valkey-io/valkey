# Tests for systemd socket activation.
#
# Simulates socket activation using systemd-socket-activate (part of the
# systemd package).  The tests are skipped automatically when either:
#   - systemd-socket-activate is not in PATH, or
#   - systemd-socket-activate does not support --now (added in systemd 258), or
#   - valkey-server was not compiled with libsystemd (HAVE_LIBSYSTEMD).

if {[auto_execok systemd-socket-activate] eq {}} {
    return
}

# --now is required so systemd-socket-activate execs the service immediately
# rather than waiting for a connection.
if {[catch {exec systemd-socket-activate --help 2>@1} help_out] ||
    ![string match "*--now*" $help_out]} {
    return
}

if {[catch {exec ldd $::VALKEY_SERVER_BIN} ldd_out] ||
    ![string match "*libsystemd*" $ldd_out]} {
    return
}

proc sa_write_config {cfgfile port datadir logfile {unixsock {}}} {
    set f [open $cfgfile w]
    puts $f "port $port"
    puts $f "bind 127.0.0.1"
    puts $f "daemonize no"
    puts $f "loglevel verbose"
    puts $f "logfile $logfile"
    puts $f "dir $datadir"
    puts $f "protected-mode no"
    if {$unixsock ne {}} {
        puts $f "unixsocket $unixsock"
    }
    close $f
}

proc sa_start {cfgfile port {unixsock {}}} {
    set args [list systemd-socket-activate --now -l "127.0.0.1:$port"]
    if {$unixsock ne {}} {
        lappend args -l $unixsock
    }
    lappend args -- $::VALKEY_SERVER_BIN $cfgfile
    set fd [open "|$args" r]
    fconfigure $fd -blocking 0
    return $fd
}

proc sa_ping_tcp {port} {
    catch {exec $::VALKEY_CLI_BIN -p $port PING} r
    return $r
}

proc sa_ping_unix {sock} {
    catch {exec $::VALKEY_CLI_BIN -s $sock PING} r
    return $r
}

proc sa_find_free_port {} {
    set s [socket -server {} 0]
    set port [lindex [fconfigure $s -sockname] 2]
    close $s
    return $port
}

proc sa_read_file {path} {
    if {[catch {open $path r} f]} { return "" }
    set data [read $f]
    close $f
    return $data
}

# ---------------------------------------------------------------------------
# Test 1: TCP socket activation
# ---------------------------------------------------------------------------
test {Socket activation: TCP listener adopted from systemd} {
    set port    [sa_find_free_port]
    set dir     [file normalize [tmpdir socket-activation-tcp]]
    set logfile [file join $dir server.log]
    set cfgfile [file join $dir valkey.conf]

    sa_write_config $cfgfile $port $dir $logfile
    set fd [sa_start $cfgfile $port]

    wait_for_condition 50 100 {
        [sa_ping_tcp $port] eq "PONG"
    } else {
        fail "valkey-server did not respond to PING on port $port"
    }

    set result [sa_ping_tcp $port]

    catch {exec $::VALKEY_CLI_BIN -p $port SHUTDOWN NOSAVE} _
    after 500
    catch {close $fd} _

    set adopted [string match "*Systemd socket activation: adopted*" [sa_read_file $logfile]]

    list $result $adopted
} {PONG 1}

# ---------------------------------------------------------------------------
# Test 2: Unix socket activation + systemd-owned path must survive shutdown
# ---------------------------------------------------------------------------
test {Socket activation: unix socket adopted and not unlinked on shutdown} {
    set port    [sa_find_free_port]
    set dir     [file normalize [tmpdir socket-activation-unix]]
    # sockaddr_un.sun_path is limited to 108 bytes; an absolute path under
    # tests/tmp/... is typically too long, so use a short fixed-root path.
    set sock    "/tmp/valkey-sa-[pid]-[clock microseconds].sock"
    set logfile [file join $dir server.log]
    set cfgfile [file join $dir valkey.conf]

    sa_write_config $cfgfile $port $dir $logfile $sock

    set fd [sa_start $cfgfile $port $sock]

    wait_for_condition 50 100 {
        [sa_ping_unix $sock] eq "PONG"
    } else {
        fail "valkey-server did not respond to PING on unix socket $sock"
    }

    set ping_result [sa_ping_unix $sock]

    catch {exec $::VALKEY_CLI_BIN -s $sock SHUTDOWN NOSAVE} _
    after 500

    # Systemd owns the unix socket; valkey must not unlink it on shutdown.
    set sock_exists [file exists $sock]

    catch {close $fd} _
    catch {file delete -force $sock} _

    list $ping_result $sock_exists
} {PONG 1}

# ---------------------------------------------------------------------------
# Test 3: Bind-address mismatch — inherited fd bound to all interfaces is
# rejected when the server is configured with bind 127.0.0.1 only.
# ---------------------------------------------------------------------------
test {Socket activation: inherited fd bound to 0.0.0.0 is rejected when bind=127.0.0.1} {
    set port    [sa_find_free_port]
    set dir     [file normalize [tmpdir socket-activation-bindmismatch]]
    set logfile [file join $dir server.log]
    set cfgfile [file join $dir valkey.conf]

    sa_write_config $cfgfile $port $dir $logfile

    # systemd passes 0.0.0.0:$port (wildcard), but valkey config binds only 127.0.0.1.
    # Expect valkey to close the mismatched fd and self-bind on 127.0.0.1:$port.
    set args [list systemd-socket-activate --now -l "0.0.0.0:$port" -- $::VALKEY_SERVER_BIN $cfgfile]
    set fd [open "|$args" r]
    fconfigure $fd -blocking 0

    wait_for_condition 50 100 {
        [sa_ping_tcp $port] eq "PONG"
    } else {
        fail "valkey-server did not respond to PING on port $port"
    }

    set result [sa_ping_tcp $port]

    catch {exec $::VALKEY_CLI_BIN -p $port SHUTDOWN NOSAVE} _
    after 500
    catch {close $fd} _

    set log [sa_read_file $logfile]
    set rejected [string match "*doesn't match any configured listener*" $log]
    set adopted  [string match "*Systemd socket activation: adopted*" $log]

    list $result $rejected $adopted
} {PONG 1 0}

# ---------------------------------------------------------------------------
# Test 4: Runtime CONFIG SET port is refused on an inherited TCP listener.
# ---------------------------------------------------------------------------
test {Socket activation: CONFIG SET port is refused on inherited listener} {
    set port    [sa_find_free_port]
    set newport [sa_find_free_port]
    set dir     [file normalize [tmpdir socket-activation-configset]]
    set logfile [file join $dir server.log]
    set cfgfile [file join $dir valkey.conf]

    sa_write_config $cfgfile $port $dir $logfile
    set fd [sa_start $cfgfile $port]

    wait_for_condition 50 100 {
        [sa_ping_tcp $port] eq "PONG"
    } else {
        fail "valkey-server did not respond to PING on port $port"
    }

    # valkey-cli prints the server error to stdout and exits 0; capture it.
    catch {exec $::VALKEY_CLI_BIN -p $port CONFIG SET port $newport} cli_out
    set cli_error [string match "*CONFIG SET failed*" $cli_out]

    catch {exec $::VALKEY_CLI_BIN -p $port SHUTDOWN NOSAVE} _
    after 500
    catch {close $fd} _

    set log [sa_read_file $logfile]
    set log_has_warning [string match "*Cannot reconfigure*inherited from systemd*" $log]

    list $cli_error $log_has_warning
} {1 1}

# ---------------------------------------------------------------------------
# Test 5: Without socket activation the server binds normally
# ---------------------------------------------------------------------------
test {Socket activation: server self-binds when no inherited fds} {
    set port    [sa_find_free_port]
    set dir     [file normalize [tmpdir socket-activation-none]]
    set logfile [file join $dir server.log]
    set cfgfile [file join $dir valkey.conf]

    sa_write_config $cfgfile $port $dir $logfile

    set fd [open "|[list $::VALKEY_SERVER_BIN $cfgfile]" r]
    fconfigure $fd -blocking 0

    wait_for_condition 50 100 {
        [sa_ping_tcp $port] eq "PONG"
    } else {
        fail "valkey-server did not respond to PING on port $port"
    }

    set result [sa_ping_tcp $port]

    catch {exec $::VALKEY_CLI_BIN -p $port SHUTDOWN NOSAVE} _
    after 500
    catch {close $fd} _

    set activated [string match "*Systemd socket activation*" [sa_read_file $logfile]]

    list $result $activated
} {PONG 0}
