project(rdma-test)

# Make sure RDMA build is enabled
if (BUILD_RDMA_MODULE)
    add_executable(rdma-test "${VALKEY_RDMA_MODULE_TEST_SRCS}")
    target_link_libraries(rdma-test "${RDMA_LIBS}")
    valkey_install_bin(rdma-test)
endif ()
