project(valkey-server)

set(INSTALL_BIN_PATH ${CMAKE_INSTALL_PREFIX}/bin)
set_directory_properties(PROPERTIES CLEAN_NO_CUSTOM 1)

# Target: valkey-server
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${VALKEY_SERVER_CFLAGS}")
message(STATUS "CFLAGS: ${CMAKE_C_FLAGS}")

get_valkey_server_linker_option(VALKEY_SERVER_LDFLAGS)
list(APPEND SERVER_LIBS "fpconv")
list(APPEND SERVER_LIBS "lualib")
list(APPEND SERVER_LIBS "hdr_histogram")
valkey_build_and_install_bin(valkey-server "${VALKEY_SERVER_SRCS}" "${VALKEY_SERVER_LDFLAGS}" "${SERVER_LIBS}"
                             "redis-server")
add_dependencies(valkey-server generate_commands_def)
add_dependencies(valkey-server generate_fmtargs_h)
add_dependencies(valkey-server release_header)

if (VALKEY_RELEASE_BUILD)
    # Enable LTO for Release build
    set_property(TARGET valkey-server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()

if (DEBUG_FORCE_DEFRAG)
    message(STATUS "Forcing Active Defrag run on valkey-server")
    target_compile_definitions(valkey-server PRIVATE DEBUG_FORCE_DEFRAG)
    target_compile_definitions(valkey-server PRIVATE HAVE_DEFRAG)
endif ()

if (BUILD_SANITIZER)
    # 'BUILD_SANITIZER' is defined in ValkeySetup module (based on user input)
    # If defined, the variables 'VALKEY_SANITAIZER_CFLAGS' and 'VALKEY_SANITAIZER_LDFLAGS'
    # are set with the link & compile flags required
    message(STATUS "Adding sanitizer flags for target valkey-server")
    target_compile_options(valkey-server PRIVATE ${VALKEY_SANITAIZER_CFLAGS})
    target_link_options(valkey-server PRIVATE ${VALKEY_SANITAIZER_LDFLAGS})
endif ()
unset(BUILD_SANITIZER CACHE)

# Target: valkey-cli
list(APPEND CLI_LIBS "linenoise")
valkey_build_and_install_bin(valkey-cli "${VALKEY_CLI_SRCS}" "${VALKEY_SERVER_LDFLAGS}" "${CLI_LIBS}" "redis-cli")
add_dependencies(valkey-cli generate_commands_def)
add_dependencies(valkey-cli generate_fmtargs_h)

# Target: valkey-benchmark
list(APPEND BENCH_LIBS "hdr_histogram")
valkey_build_and_install_bin(valkey-benchmark "${VALKEY_BENCHMARK_SRCS}" "${VALKEY_SERVER_LDFLAGS}" "${BENCH_LIBS}"
                             "redis-benchmark")
add_dependencies(valkey-benchmark generate_commands_def)
add_dependencies(valkey-benchmark generate_fmtargs_h)

# Targets: valkey-sentinel, valkey-check-aof and valkey-check-rdb are just symbolic links
valkey_create_symlink("valkey-server" "valkey-sentinel")
valkey_create_symlink("valkey-server" "valkey-check-rdb")
valkey_create_symlink("valkey-server" "valkey-check-aof")

# Target valkey-rdma
if (BUILD_RDMA_MODULE)
    set(MODULE_NAME "valkey-rdma")
    message(STATUS "Building RDMA module")
    add_library(${MODULE_NAME} SHARED "${VALKEY_RDMA_MODULE_SRCS}")
    target_compile_options(${MODULE_NAME} PRIVATE -DBUILD_RDMA_MODULE=2 -DUSE_RDMA=1)
    target_link_libraries(${MODULE_NAME} "${RDMA_LIBS}")
    # remove the "lib" prefix from the module
    set_target_properties(${MODULE_NAME} PROPERTIES PREFIX "")
    valkey_install_bin(${MODULE_NAME})
endif ()

# Target valkey-tls (a module)
if (BUILD_TLS_MODULE)
    message(STATUS "Building TLS as a module")
    set(MODULE_NAME "valkey-tls")
    add_library(${MODULE_NAME} SHARED ${VALKEY_TLS_MODULE_SRCS})
    target_compile_options(${MODULE_NAME} PRIVATE -DUSE_OPENSSL=2 -DBUILD_TLS_MODULE=2)
    if (APPLE)
        # Some symbols can only be resolved during runtime (they exist in the executable)
        target_link_options(${MODULE_NAME} PRIVATE -undefined dynamic_lookup)
    endif ()
    target_link_libraries(${MODULE_NAME} hiredis_ssl OpenSSL::SSL)
    set_target_properties(${MODULE_NAME} PROPERTIES PREFIX "")
endif ()

if (BUILD_EXAMPLE_MODULES)
    # Include the modules ("hello*")
    message(STATUS "Building example modules")
    add_subdirectory(modules)
endif ()

if (BUILD_UNIT_TESTS)
    add_subdirectory(unit)
endif ()
