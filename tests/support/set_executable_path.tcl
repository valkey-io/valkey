# Set the directory to find Valkey binaries for tests. Historically we've been
# using make to build binaries under the src/ directory. Since we start supporting
# CMake as well, we allow changing base dir by passing ENV variable `VALKEY_BIN_DIR`
set ::VALKEY_BIN_DIR [expr {[info exists ::env(VALKEY_BIN_DIR)] ? $::env(VALKEY_BIN_DIR) : "src"}]
puts "::VALKEY_BIN_DIR is $::VALKEY_BIN_DIR"

# Helper to build absolute paths
proc __build_absolute_path {name} {
    set p [file join $::VALKEY_BIN_DIR $name]
    if {![file executable $p]} {
        error "Binary not found or not executable: $p (VALKEY_BIN_DIR=$::VALKEY_BIN_DIR)"
    }
    return $p
}

set ::VALKEY_SERVER_BIN    [__build_absolute_path "valkey-server"]
set ::VALKEY_CLI_BIN       [__build_absolute_path "valkey-cli"]
set ::VALKEY_BENCHMARK_BIN [__build_absolute_path "valkey-benchmark"]
set ::VALKEY_CHECK_AOF_BIN [__build_absolute_path "valkey-check-aof"]
set ::VALKEY_CHECK_RDB_BIN [__build_absolute_path "valkey-check-rdb"]
set ::VALKEY_SENTINEL_BIN  [__build_absolute_path "valkey-sentinel"]