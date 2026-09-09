add_subdirectory(rdma)

if (BUILD_TEST_MODULES)
    add_subdirectory(modules)
endif ()
