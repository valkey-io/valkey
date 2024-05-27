proc valkeybenchmark_tls_config {testsdir} {
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

proc valkeybenchmark {host port {opts {}}} {
    set cmd [list src/valkey-benchmark -h $host -p $port]
    lappend cmd {*}[valkeybenchmark_tls_config "tests"]
    lappend cmd {*}$opts
    return $cmd
}

proc valkeybenchmarkuri {host port {opts {}}} {
    set cmd [list src/valkey-benchmark -u valkey://$host:$port]
    lappend cmd {*}[valkeybenchmark_tls_config "tests"]
    lappend cmd {*}$opts
    return $cmd
}

proc valkeybenchmarkuriuserpass {host port user pass {opts {}}} {
    set cmd [list src/valkey-benchmark -u valkey://$user:$pass@$host:$port]
    lappend cmd {*}[valkeybenchmark_tls_config "tests"]
    lappend cmd {*}$opts
    return $cmd
}
