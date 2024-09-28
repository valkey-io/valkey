project(valkey-server)

set(INSTALL_BIN_PATH ${CMAKE_INSTALL_PREFIX}/bin)

# Create a library from Valkey sources
add_library(valkeylib STATIC "${VALKEY_LIB_SRCS}")

if (BUILD_TESTS)
    # Build unit tests only
    add_definitions(-DSERVER_TEST=1)
    add_subdirectory(unit)
else ()
    # Target: valkey-server
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${VALKEY_SERVER_CFLAGS}")
    message(STATUS "CFLAGS: ${CMAKE_C_FLAGS}")

    get_valkey_server_linker_option(VALKEY_SERVER_LDFLAGS)
    list(APPEND SERVER_LIBS "valkeylib")
    list(APPEND SERVER_LIBS "fpconv")
    list(APPEND SERVER_LIBS "lualib")
    list(APPEND SERVER_LIBS "hdr_histogram")
    list(APPEND SERVER_LIBS "hiredis")
    set(VALKEY_SERVER_SRCS "${CMAKE_SOURCE_DIR}/src/server.c")
    valkey_build_and_install_bin(valkey-server "${VALKEY_SERVER_SRCS}" "${VALKEY_SERVER_LDFLAGS}" "${SERVER_LIBS}"
                                 "redis-server")
    add_dependencies(valkey-server release_header)

    # Target: valkey-cli
    list(APPEND CLI_LIBS "hiredis")
    list(APPEND CLI_LIBS "linenoise")
    valkey_build_and_install_bin(valkey-cli "${VALKEY_CLI_SRCS}" "${VALKEY_SERVER_LDFLAGS}" "${CLI_LIBS}" "redis-cli")

    # Target: valkey-benchmark
    list(APPEND BENCH_LIBS "hiredis")
    list(APPEND BENCH_LIBS "hdr_histogram")
    valkey_build_and_install_bin(valkey-benchmark "${VALKEY_BENCHMARK_SRCS}" "${VALKEY_SERVER_LDFLAGS}" "${BENCH_LIBS}"
                                 "redis-benchmark")

    # Targets: valkey-sentinel, valkey-check-aof and valkey-check-rdb are just symbolic links
    valkey_create_symlink("${INSTALL_BIN_PATH}/valkey-server" "${INSTALL_BIN_PATH}/valkey-sentinel")
    valkey_create_symlink("${INSTALL_BIN_PATH}/valkey-server" "${INSTALL_BIN_PATH}/valkey-check-rdb")
    valkey_create_symlink("${INSTALL_BIN_PATH}/valkey-server" "${INSTALL_BIN_PATH}/valkey-check-aof")
endif ()
