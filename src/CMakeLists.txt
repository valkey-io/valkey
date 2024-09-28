project(valkey-server)

# Target: valkey-server
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${VALKEY_SERVER_CFLAGS}")
message(STATUS "CFLAGS: ${CMAKE_C_FLAGS}")

add_executable(valkey-server ${VALKEY_SERVER_SRCS})

# Get the linker options (defined by ValkeySetup.cmake module)
get_valkey_server_linker_option(VALKEY_SERVER_LDFLAGS)
message(STATUS "LDFLAGS: ${VALKEY_SERVER_LDFLAGS}")

target_link_libraries(valkey-server ${VALKEY_SERVER_LDFLAGS} fpconv lualib hdr_histogram hiredis)

if (USE_JEMALLOC)
    # Using jemalloc
    target_link_libraries(valkey-server jemalloc)
endif ()

if (USE_TLS)
    # Add required libraries needed for TLS valkey
    target_link_libraries(valkey-server OpenSSL::SSL hiredis_ssl)
endif ()

# Make sure that release_header is executed before the valkey-server target
add_dependencies(valkey-server release_header)

# Target: valkey-cli
add_executable(valkey-cli ${VALKEY_CLI_SRCS})
target_link_libraries(valkey-cli ${VALKEY_SERVER_LDFLAGS} hiredis linenoise)
if (USE_TLS)
    # Add required libraries needed for TLS valkey-cli
    target_link_libraries(valkey-cli OpenSSL::SSL hiredis_ssl)
endif ()

if (USE_JEMALLOC)
    # Using jemalloc
    target_link_libraries(valkey-cli jemalloc)
endif ()
