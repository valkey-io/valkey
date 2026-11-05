if (USE_SYSTEM_JEMALLOC_LIB)
    # Link the jemalloc installed on the system instead of the vendored copy. The target keeps the name of the
    # vendored one so that everything linking "jemalloc" works unchanged.
    find_path(SYSTEM_JEMALLOC_INCLUDE_DIR jemalloc/jemalloc.h REQUIRED)
    find_library(SYSTEM_JEMALLOC_LIBRARY jemalloc REQUIRED)
    message(STATUS "Using system jemalloc: ${SYSTEM_JEMALLOC_LIBRARY}")
    add_library(jemalloc UNKNOWN IMPORTED GLOBAL)
    set_target_properties(jemalloc PROPERTIES IMPORTED_LOCATION "${SYSTEM_JEMALLOC_LIBRARY}"
                                              INTERFACE_INCLUDE_DIRECTORIES "${SYSTEM_JEMALLOC_INCLUDE_DIR}")
elseif (USE_JEMALLOC)
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
    if (USE_RDMA EQUAL 2) # Module
        set(ENABLE_DLOPEN_RDMA ON CACHE BOOL "Build valkey_rdma with dynamic loading")
    endif()
endif ()
# Let libvalkey use sds provided by valkey.
set(SDS_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src)

add_subdirectory(libvalkey)
add_subdirectory(linenoise)
add_subdirectory(fpconv)
add_subdirectory(hdr_histogram)
add_subdirectory(fast_float)
add_subdirectory(lz4)

# Clear any cached variables passed to libvalkey from the cache
unset(BUILD_SHARED_LIBS CACHE)
unset(DISABLE_TESTS CACHE)
unset(ENABLE_TLS CACHE)
unset(ENABLE_RDMA CACHE)
