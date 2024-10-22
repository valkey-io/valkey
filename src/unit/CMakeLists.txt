project(valkey-unit-tests)

file(GLOB UNIT_TEST_SRCS "${CMAKE_CURRENT_LIST_DIR}/*.c")
set(UNIT_TEST_SRCS "${UNIT_TEST_SRCS}")

get_valkey_server_linker_option(VALKEY_SERVER_LDFLAGS)

# Build Valkey sources as a static library for the test
add_library(valkeylib STATIC ${VALKEY_SERVER_SRCS})
target_compile_options(valkeylib PRIVATE "-std=gnu11")

add_executable(valkey-unit-tests ${UNIT_TEST_SRCS})
add_dependencies(valkey-unit-tests generate_test_files_h)

target_compile_options(valkey-unit-tests PRIVATE "-std=gnu11")
if (UNIX AND NOT APPLE)
    # Avoid duplicate symbols on non macOS
    target_link_options(valkey-unit-tests PRIVATE "-Wl,--allow-multiple-definition")
endif ()

if (USE_TLS)
    # Add required libraries needed for TLS
    target_link_libraries(valkey-unit-tests OpenSSL::SSL hiredis_ssl)
endif ()

if (USE_JEMALLOC)
    # Using jemalloc
    target_link_libraries(valkey-unit-tests jemalloc)
endif ()

# Place this line last to ensure that ${ld_flags} is placed last on the linker line
target_link_libraries(
    valkey-unit-tests
    valkeylib
    fpconv
    lualib
    hdr_histogram
    hiredis
    ${VALKEY_SERVER_LDFLAGS})
