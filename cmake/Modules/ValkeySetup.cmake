set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Generate compile_commands.json file for IDEs code completion support
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

set(VALKEY_SERVER_CFLAGS "")
set(VALKEY_SERVER_LDFLAGS "")

macro (add_valkey_server_compiler_options value)
    set(VALKEY_SERVER_CFLAGS "${VALKEY_SERVER_CFLAGS} ${value}")
endmacro ()

macro (add_valkey_server_linker_option value)
    list(APPEND VALKEY_SERVER_LDFLAGS ${value})
endmacro ()

# Options
option(WITH_TLS "Build valkey-server with TLS support" OFF)

function (get_valkey_server_linker_option return_value)
    list(JOIN VALKEY_SERVER_LDFLAGS " " ${value} ${return_value})
endfunction ()

# Installed executables will have this permissions
set(VALKEY_EXE_PERMISSIONS
    OWNER_EXECUTE
    OWNER_WRITE
    OWNER_READ
    GROUP_EXECUTE
    GROUP_READ
    WORLD_EXECUTE
    WORLD_READ)

if (NOT WITH_MALLOC)
    if (APPLE)
        set(WITH_MALLOC "libc")
    elseif (UNIX)
        set(WITH_MALLOC "jemalloc")
    endif ()
endif ()

# User may pass different allocator library. Using -DWITH_MALLOC=<libname>, make sure it is a valid value
if (WITH_MALLOC)
    if ("${WITH_MALLOC}" STREQUAL "jemalloc")
        set(MALLOC_LIB "jemalloc")
        add_valkey_server_compiler_options("-DUSE_JEMALLOC")
        set(USE_JEMALLOC 1)
    elseif ("${WITH_MALLOC}" STREQUAL "libc")
        set(MALLOC_LIB "libc")
    elseif ("${WITH_MALLOC}" STREQUAL "tcmalloc")
        set(MALLOC_LIB "tcmalloc")
        add_valkey_server_compiler_options("-DUSE_TCMALLOC")
    elseif ("${WITH_MALLOC}" STREQUAL "tcmalloc_minimal")
        set(MALLOC_LIB "tcmalloc_minimal")
        add_valkey_server_compiler_options("-DUSE_TCMALLOC")
    else ()
        message(FATAL_ERROR "WITH_MALLOC can be one of: jemalloc, libc, tcmalloc or tcmalloc_minimal")
    endif ()
    unset(WITH_MALLOC CACHE)
    unset(USE_JEMALLOC CACHE)
endif ()

message(STATUS "Using ${MALLOC_LIB}")

# TLS support
if (WITH_TLS)
    find_package(OpenSSL REQUIRED)
    message(STATUS "OpenSSL include dir: ${OPENSSL_INCLUDE_DIR}")
    message(STATUS "OpenSSL libraries: ${OPENSSL_LIBRARIES}")

    include_directories(${OPENSSL_INCLUDE_DIR})
    add_valkey_server_compiler_options("-DUSE_OPENSSL=1")
    add_valkey_server_compiler_options("-DBUILD_TLS_MODULE=0")
    set(USE_TLS 1)
    unset(WITH_TLS CACHE)
endif ()

set(BUILDING_ARM64 0)
set(BUILDING_ARM32 0)

if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "arm64")
    set(BUILDING_ARM64 1)
endif ()

if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "arm")
    set(BUILDING_ARM32 1)
endif ()

message(STATUS "Building on ${CMAKE_HOST_SYSTEM_NAME}")
if (BUILDING_ARM64)
    message(STATUS "Compiling valkey for ARM64")
    add_valkey_server_linker_option("-funwind-tables")
endif ()

if (APPLE)
    add_valkey_server_linker_option("-ldl")
elseif (UNIX)
    add_valkey_server_linker_option("-pthread")
    add_valkey_server_linker_option("-ldl")
    add_valkey_server_linker_option("-lm")
endif ()

# Sanitizer
if (WITH_SANITIZER)
    # For best results, force libc
    set(MALLOC_LIB, "libc")
    if ("${WITH_SANITIZER}" STREQUAL "address")
        add_valkey_server_compiler_options("-fsanitize=address -fno-sanitize-recover=all -fno-omit-frame-pointer")
        add_valkey_server_linker_option("-fsanitize=address")
    elseif ("${WITH_SANITIZER}" STREQUAL "thread")
        add_valkey_server_compiler_options("-fsanitize=thread -fno-sanitize-recover=all -fno-omit-frame-pointer")
        add_valkey_server_linker_option("-fsanitize=thread")
    elseif ("${WITH_SANITIZER}" STREQUAL "undefined")
        add_valkey_server_compiler_options("-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer")
        add_valkey_server_linker_option("-fsanitize=undefined")
    else ()
        message(FATAL_ERROR "Unknown sanitizer: ${WITH_SANITIZER}")
    endif ()
endif ()

include_directories("${CMAKE_SOURCE_DIR}/deps/hiredis")
include_directories("${CMAKE_SOURCE_DIR}/deps/linenoise")
include_directories("${CMAKE_SOURCE_DIR}/deps/lua/src")
include_directories("${CMAKE_SOURCE_DIR}/deps/hdr_histogram")
include_directories("${CMAKE_SOURCE_DIR}/deps/fpconv")

add_subdirectory("${CMAKE_SOURCE_DIR}/deps")

# Update linker flags for the allocator
if (USE_JEMALLOC)
    include_directories("${CMAKE_SOURCE_DIR}/deps/jemalloc/include")
endif ()

# Build the source file list
set(VALKEY_SERVER_SRCS
    ${CMAKE_SOURCE_DIR}/src/threads_mngr.c
    ${CMAKE_SOURCE_DIR}/src/adlist.c
    ${CMAKE_SOURCE_DIR}/src/quicklist.c
    ${CMAKE_SOURCE_DIR}/src/ae.c
    ${CMAKE_SOURCE_DIR}/src/anet.c
    ${CMAKE_SOURCE_DIR}/src/dict.c
    ${CMAKE_SOURCE_DIR}/src/kvstore.c
    ${CMAKE_SOURCE_DIR}/src/server.c
    ${CMAKE_SOURCE_DIR}/src/sds.c
    ${CMAKE_SOURCE_DIR}/src/zmalloc.c
    ${CMAKE_SOURCE_DIR}/src/lzf_c.c
    ${CMAKE_SOURCE_DIR}/src/lzf_d.c
    ${CMAKE_SOURCE_DIR}/src/pqsort.c
    ${CMAKE_SOURCE_DIR}/src/zipmap.c
    ${CMAKE_SOURCE_DIR}/src/sha1.c
    ${CMAKE_SOURCE_DIR}/src/ziplist.c
    ${CMAKE_SOURCE_DIR}/src/release.c
    ${CMAKE_SOURCE_DIR}/src/memory_prefetch.c
    ${CMAKE_SOURCE_DIR}/src/io_threads.c
    ${CMAKE_SOURCE_DIR}/src/networking.c
    ${CMAKE_SOURCE_DIR}/src/util.c
    ${CMAKE_SOURCE_DIR}/src/object.c
    ${CMAKE_SOURCE_DIR}/src/db.c
    ${CMAKE_SOURCE_DIR}/src/replication.c
    ${CMAKE_SOURCE_DIR}/src/rdb.c
    ${CMAKE_SOURCE_DIR}/src/t_string.c
    ${CMAKE_SOURCE_DIR}/src/t_list.c
    ${CMAKE_SOURCE_DIR}/src/t_set.c
    ${CMAKE_SOURCE_DIR}/src/t_zset.c
    ${CMAKE_SOURCE_DIR}/src/t_hash.c
    ${CMAKE_SOURCE_DIR}/src/config.c
    ${CMAKE_SOURCE_DIR}/src/aof.c
    ${CMAKE_SOURCE_DIR}/src/pubsub.c
    ${CMAKE_SOURCE_DIR}/src/multi.c
    ${CMAKE_SOURCE_DIR}/src/debug.c
    ${CMAKE_SOURCE_DIR}/src/sort.c
    ${CMAKE_SOURCE_DIR}/src/intset.c
    ${CMAKE_SOURCE_DIR}/src/syncio.c
    ${CMAKE_SOURCE_DIR}/src/cluster.c
    ${CMAKE_SOURCE_DIR}/src/cluster_legacy.c
    ${CMAKE_SOURCE_DIR}/src/cluster_slot_stats.c
    ${CMAKE_SOURCE_DIR}/src/crc16.c
    ${CMAKE_SOURCE_DIR}/src/endianconv.c
    ${CMAKE_SOURCE_DIR}/src/slowlog.c
    ${CMAKE_SOURCE_DIR}/src/eval.c
    ${CMAKE_SOURCE_DIR}/src/bio.c
    ${CMAKE_SOURCE_DIR}/src/rio.c
    ${CMAKE_SOURCE_DIR}/src/rand.c
    ${CMAKE_SOURCE_DIR}/src/memtest.c
    ${CMAKE_SOURCE_DIR}/src/syscheck.c
    ${CMAKE_SOURCE_DIR}/src/crcspeed.c
    ${CMAKE_SOURCE_DIR}/src/crccombine.c
    ${CMAKE_SOURCE_DIR}/src/crc64.c
    ${CMAKE_SOURCE_DIR}/src/bitops.c
    ${CMAKE_SOURCE_DIR}/src/sentinel.c
    ${CMAKE_SOURCE_DIR}/src/notify.c
    ${CMAKE_SOURCE_DIR}/src/setproctitle.c
    ${CMAKE_SOURCE_DIR}/src/blocked.c
    ${CMAKE_SOURCE_DIR}/src/hyperloglog.c
    ${CMAKE_SOURCE_DIR}/src/latency.c
    ${CMAKE_SOURCE_DIR}/src/sparkline.c
    ${CMAKE_SOURCE_DIR}/src/valkey-check-rdb.c
    ${CMAKE_SOURCE_DIR}/src/valkey-check-aof.c
    ${CMAKE_SOURCE_DIR}/src/geo.c
    ${CMAKE_SOURCE_DIR}/src/lazyfree.c
    ${CMAKE_SOURCE_DIR}/src/module.c
    ${CMAKE_SOURCE_DIR}/src/evict.c
    ${CMAKE_SOURCE_DIR}/src/expire.c
    ${CMAKE_SOURCE_DIR}/src/geohash.c
    ${CMAKE_SOURCE_DIR}/src/geohash_helper.c
    ${CMAKE_SOURCE_DIR}/src/childinfo.c
    ${CMAKE_SOURCE_DIR}/src/defrag.c
    ${CMAKE_SOURCE_DIR}/src/siphash.c
    ${CMAKE_SOURCE_DIR}/src/rax.c
    ${CMAKE_SOURCE_DIR}/src/t_stream.c
    ${CMAKE_SOURCE_DIR}/src/listpack.c
    ${CMAKE_SOURCE_DIR}/src/localtime.c
    ${CMAKE_SOURCE_DIR}/src/lolwut.c
    ${CMAKE_SOURCE_DIR}/src/lolwut5.c
    ${CMAKE_SOURCE_DIR}/src/lolwut6.c
    ${CMAKE_SOURCE_DIR}/src/acl.c
    ${CMAKE_SOURCE_DIR}/src/tracking.c
    ${CMAKE_SOURCE_DIR}/src/socket.c
    ${CMAKE_SOURCE_DIR}/src/tls.c
    ${CMAKE_SOURCE_DIR}/src/sha256.c
    ${CMAKE_SOURCE_DIR}/src/timeout.c
    ${CMAKE_SOURCE_DIR}/src/setcpuaffinity.c
    ${CMAKE_SOURCE_DIR}/src/monotonic.c
    ${CMAKE_SOURCE_DIR}/src/mt19937-64.c
    ${CMAKE_SOURCE_DIR}/src/resp_parser.c
    ${CMAKE_SOURCE_DIR}/src/call_reply.c
    ${CMAKE_SOURCE_DIR}/src/script_lua.c
    ${CMAKE_SOURCE_DIR}/src/script.c
    ${CMAKE_SOURCE_DIR}/src/functions.c
    ${CMAKE_SOURCE_DIR}/src/function_lua.c
    ${CMAKE_SOURCE_DIR}/src/commands.c
    ${CMAKE_SOURCE_DIR}/src/strl.c
    ${CMAKE_SOURCE_DIR}/src/connection.c
    ${CMAKE_SOURCE_DIR}/src/unix.c
    ${CMAKE_SOURCE_DIR}/src/logreqres.c)

set(VALKEY_CLI_SRCS
    ${CMAKE_SOURCE_DIR}/src/anet.c
    ${CMAKE_SOURCE_DIR}/src/adlist.c
    ${CMAKE_SOURCE_DIR}/src/dict.c
    ${CMAKE_SOURCE_DIR}/src/valkey-cli.c
    ${CMAKE_SOURCE_DIR}/src/zmalloc.c
    ${CMAKE_SOURCE_DIR}/src/release.c
    ${CMAKE_SOURCE_DIR}/src/ae.c
    ${CMAKE_SOURCE_DIR}/src/serverassert.c
    ${CMAKE_SOURCE_DIR}/src/crcspeed.c
    ${CMAKE_SOURCE_DIR}/src/crccombine.c
    ${CMAKE_SOURCE_DIR}/src/crc64.c
    ${CMAKE_SOURCE_DIR}/src/siphash.c
    ${CMAKE_SOURCE_DIR}/src/crc16.c
    ${CMAKE_SOURCE_DIR}/src/monotonic.c
    ${CMAKE_SOURCE_DIR}/src/cli_common.c
    ${CMAKE_SOURCE_DIR}/src/mt19937-64.c
    ${CMAKE_SOURCE_DIR}/src/strl.c
    ${CMAKE_SOURCE_DIR}/src/cli_commands.c)

set(VALKEY_BENCHMARK_SRCS
    ${CMAKE_SOURCE_DIR}/src/ae.c
    ${CMAKE_SOURCE_DIR}/src/anet.c
    ${CMAKE_SOURCE_DIR}/src/valkey-benchmark.c
    ${CMAKE_SOURCE_DIR}/src/adlist.c
    ${CMAKE_SOURCE_DIR}/src/dict.c
    ${CMAKE_SOURCE_DIR}/src/zmalloc.c
    ${CMAKE_SOURCE_DIR}/src/serverassert.c
    ${CMAKE_SOURCE_DIR}/src/release.c
    ${CMAKE_SOURCE_DIR}/src/crcspeed.c
    ${CMAKE_SOURCE_DIR}/src/crccombine.c
    ${CMAKE_SOURCE_DIR}/src/crc64.c
    ${CMAKE_SOURCE_DIR}/src/siphash.c
    ${CMAKE_SOURCE_DIR}/src/crc16.c
    ${CMAKE_SOURCE_DIR}/src/monotonic.c
    ${CMAKE_SOURCE_DIR}/src/cli_common.c
    ${CMAKE_SOURCE_DIR}/src/mt19937-64.c
    ${CMAKE_SOURCE_DIR}/src/strl.c)

add_custom_target(
    release_header
    COMMAND sh -c '${CMAKE_SOURCE_DIR}/src/mkreleasehdr.sh'
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src")

# Common compiler flags
add_valkey_server_compiler_options("-pedantic")

unset(CMAKE_C_FLAGS CACHE)
unset(WITH_SANITIZER CACHE)
unset(VALKEY_SERVER_LDFLAGS CACHE)
unset(VALKEY_SERVER_CFLAGS CACHE)

# Helper macro for creating symbolic link so that: link -> source
macro (valkey_create_symlink source link)
    install(
        CODE "execute_process( \
    COMMAND ${CMAKE_COMMAND} -E create_symlink \
    ${source} \
    ${link}   \
    )")
endmacro ()

# Helper macro that defines, builds and installs `target` In addition, it creates a symbolic link between the target and
# `link_name`
macro (valkey_build_and_install_bin target sources ld_flags libs link_name)
    add_executable(${target} ${sources})
    if (USE_TLS)
        # Add required libraries needed for TLS
        target_link_libraries(${target} OpenSSL::SSL hiredis_ssl)
    endif ()

    if (USE_JEMALLOC)
        # Using jemalloc
        target_link_libraries(${target} jemalloc)
    endif ()

    # Place this line last to ensure that ${ld_flags} is placed last on the linker line
    target_link_libraries(${target} ${libs} ${ld_flags})

    # Install cli tool and create a redis symbolic link
    install(
        TARGETS ${target}
        DESTINATION ${INSTALL_BIN_PATH}
        PERMISSIONS ${VALKEY_EXE_PERMISSIONS})
    valkey_create_symlink(${INSTALL_BIN_PATH}/${target} ${INSTALL_BIN_PATH}/${link_name})
endmacro ()
