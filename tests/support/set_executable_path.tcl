# Executable path globals used by tests backported from newer branches.
#
# Older release branches still run binaries from src/. Define the same globals
# newer tests expect without backporting the broader CMake test refactor.
set __valkey_support_dir [file dirname [file normalize [info script]]]
set __valkey_root_dir [file dirname [file dirname $__valkey_support_dir]]

if {![info exists ::VALKEY_PROG_SUFFIX]} {
    if {[info exists ::env(VALKEY_PROG_SUFFIX)]} {
        set ::VALKEY_PROG_SUFFIX $::env(VALKEY_PROG_SUFFIX)
    } else {
        set ::VALKEY_PROG_SUFFIX ""
    }
}

if {![info exists ::VALKEY_BIN_DIR]} {
    if {[info exists ::env(VALKEY_BIN_DIR)]} {
        set ::VALKEY_BIN_DIR [file normalize $::env(VALKEY_BIN_DIR)]
    } else {
        set ::VALKEY_BIN_DIR [file join $__valkey_root_dir src]
    }
}

foreach {var name} {
    ::VALKEY_SERVER_BIN    valkey-server
    ::VALKEY_CLI_BIN       valkey-cli
    ::VALKEY_BENCHMARK_BIN valkey-benchmark
    ::VALKEY_CHECK_AOF_BIN valkey-check-aof
    ::VALKEY_CHECK_RDB_BIN valkey-check-rdb
    ::VALKEY_SENTINEL_BIN  valkey-sentinel
} {
    if {![info exists $var]} {
        set $var [file join $::VALKEY_BIN_DIR "${name}${::VALKEY_PROG_SUFFIX}"]
    }
}

if {![info exists ::VALKEY_TLS_MODULE]} {
    if {[info exists ::env(VALKEY_BIN_DIR)]} {
        set ::VALKEY_TLS_MODULE [file join [file dirname $::VALKEY_BIN_DIR] lib "valkey-tls${::VALKEY_PROG_SUFFIX}.so"]
    } else {
        set ::VALKEY_TLS_MODULE [file join $__valkey_root_dir src "valkey-tls${::VALKEY_PROG_SUFFIX}.so"]
    }
}

unset __valkey_support_dir __valkey_root_dir
