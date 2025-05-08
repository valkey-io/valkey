if (USE_JEMALLOC)
    add_subdirectory(jemalloc)
endif ()
add_subdirectory(lua)

# Set libvalkey options. We need to disable the defaults set in the OPTION(..) we do this by setting them in the CACHE
set(BUILD_SHARED_LIBS
    OFF
    CACHE BOOL "Build shared libraries")
set(DISABLE_TESTS
    ON
    CACHE BOOL "If tests should be compiled or not")
if (USE_TLS) # Module or no module
    message(STATUS "Building valkey_tls")
    set(ENABLE_TLS
        ON
        CACHE BOOL "If TLS support should be compiled or not")
endif ()
if (USE_RDMA) # Module or no module
    message(STATUS "Building valkey_rdma")
    set(ENABLE_RDMA
        ON
        CACHE BOOL "If RDMA support should be compiled or not")
endif ()
# Let libvalkey use sds and dict provided by valkey.
set(DICT_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src)
set(SDS_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src)

add_subdirectory(libvalkey)
add_subdirectory(linenoise)
add_subdirectory(fpconv)
add_subdirectory(hdr_histogram)

# Clear any cached variables passed to libvalkey from the cache
unset(BUILD_SHARED_LIBS CACHE)
unset(DISABLE_TESTS CACHE)
unset(ENABLE_TLS CACHE)
unset(ENABLE_RDMA CACHE)
