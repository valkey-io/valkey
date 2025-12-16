# Set the directory to find Valkey binaries for tests. Historically we've been
# using make to build binaries under the src/ directory. Since we start supporting
# CMake as well, we allow changing base dir by passing ENV variable `VALKEY_BIN_DIR`,
# which could be either absolute or relative path (e.g. cmake-build-debug/bin).
if {[info exists ::env(VALKEY_BIN_DIR)]} {
    set ::VALKEY_BIN_DIR [file normalize $::env(VALKEY_BIN_DIR)]
} else {
    set ::VALKEY_BIN_DIR "[pwd]/src"
}

# Optional program suffix (e.g. `make PROG_SUFFIX=-alt` will create binary valkey-server-alt).
# Passed from `make test` as environment variable VALKEY_PROG_SUFFIX.
set ::VALKEY_PROG_SUFFIX [expr {
    [info exists ::env(VALKEY_PROG_SUFFIX)] ? $::env(VALKEY_PROG_SUFFIX) : ""
}]

# Helper to build absolute paths
proc valkey_bin_absolute_path {name} {
    set full_name "${name}${::VALKEY_PROG_SUFFIX}"
    return [file join $::VALKEY_BIN_DIR $full_name]
}

set ::VALKEY_SERVER_BIN    [valkey_bin_absolute_path "valkey-server"]
set ::VALKEY_CLI_BIN       [valkey_bin_absolute_path "valkey-cli"]
set ::VALKEY_BENCHMARK_BIN [valkey_bin_absolute_path "valkey-benchmark"]
set ::VALKEY_CHECK_AOF_BIN [valkey_bin_absolute_path "valkey-check-aof"]
set ::VALKEY_CHECK_RDB_BIN [valkey_bin_absolute_path "valkey-check-rdb"]
set ::VALKEY_SENTINEL_BIN  [valkey_bin_absolute_path "valkey-sentinel"]

if {![file executable $::VALKEY_SERVER_BIN]} {
    error "Binary not found or not executable: $::VALKEY_SERVER_BIN"
}