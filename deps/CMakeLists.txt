if (USE_JEMALLOC)
    add_subdirectory(jemalloc)
endif ()
add_subdirectory(lua)

# Set hiredis options. We need to disable the defaults set in the OPTION(..) we do this by setting them in the CACHE
set(BUILD_SHARED_LIBS
    OFF
    CACHE BOOL "Build shared libraries")
set(DISABLE_TESTS
    ON
    CACHE BOOL "If tests should be compiled or not")
if (USE_TLS) # Module or no module
    message(STATUS "Building hiredis_ssl")
    set(ENABLE_SSL
        ON
        CACHE BOOL "Should we test SSL connections")
endif ()

add_subdirectory(hiredis)
add_subdirectory(linenoise)
add_subdirectory(fpconv)
add_subdirectory(hdr_histogram)

# Clear any cached variables passed to hiredis from the cache
unset(BUILD_SHARED_LIBS CACHE)
unset(DISABLE_TESTS CACHE)
unset(ENABLE_SSL CACHE)
