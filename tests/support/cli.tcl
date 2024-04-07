proc valkeycli_tls_config {testsdir} {
    set tlsdir [file join $testsdir tls]
    set cert [file join $tlsdir client.crt]
    set key [file join $tlsdir client.key]
    set cacert [file join $tlsdir ca.crt]

    if {$::tls} {
        return [list --tls --cert $cert --key $key --cacert $cacert]
    } else {
        return {}
    }
}

# Returns command line for executing valkey-cli
proc valkeycli {host port {opts {}}} {
    set cmd [list src/valkey-cli -h $host -p $port]
    lappend cmd {*}[valkeycli_tls_config "tests"]
    lappend cmd {*}$opts
    return $cmd
}

proc valkeycliuri {scheme host port {opts {}}} {
    set cmd [list src/valkey-cli -u $scheme$host:$port]
    lappend cmd {*}[valkeycli_tls_config "tests"]
    lappend cmd {*}$opts
    return $cmd
}

# Returns command line for executing valkey-cli with a unix socket address
proc valkeycli_unixsocket {unixsocket {opts {}}} {
    return [list src/valkey-cli -s $unixsocket {*}$opts]
}

# Run valkey-cli with specified args on the server of specified level.
# Returns output broken down into individual lines.
proc valkeycli_exec {level args} {
    set cmd [valkeycli_unixsocket [srv $level unixsocket] $args]
    set fd [open "|$cmd" "r"]
    set ret [lrange [split [read $fd] "\n"] 0 end-1]
    close $fd

    return $ret
}
