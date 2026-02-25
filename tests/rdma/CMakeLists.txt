project(rdma-test)

# Make sure RDMA build is enabled
if (BUILD_RDMA_MODULE)
    add_executable(rdma-test "${CMAKE_SOURCE_DIR}/tests/rdma/rdma-test.c")
    target_link_libraries(rdma-test "${RDMA_LIBS}")
    target_link_options(rdma-test PRIVATE "-pthread")
    valkey_install_bin(rdma-test)
endif ()
