add_subdirectory(jemalloc)
add_subdirectory(lua)

# Pass options to HiRedis CMakeLists.txt
set(BUILD_SHARED_LIBS OFF)
set(DISABLE_TESTS ON)
if (USE_TLS)
    message(STATUS "Building hiredis_ssl")
    set(ENABLE_SSL ON)
endif ()

add_subdirectory(hiredis)

add_subdirectory(linenoise)
add_subdirectory(fpconv)
add_subdirectory(hdr_histogram)
