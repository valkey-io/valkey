SET(TEST_WITH_VALKEY_VERSION "7.2.5" CACHE STRING "Valkey version used when running tests.")

if(ENABLE_TLS)
  # Generate TLS certs and keys when needed
  set(TLS_CONFIGS ca.crt ca.key ca.txt valkey.crt valkey.key client.crt client.key)
  add_custom_command(
    OUTPUT ${TLS_CONFIGS}
    COMMAND openssl genrsa -out ca.key 4096
    COMMAND openssl req -x509 -new -nodes -sha256 -key ca.key -days 3650 -subj '/CN=Valkey Test CA' -out ca.crt
    COMMAND openssl genrsa -out valkey.key 2048
    COMMAND openssl req -new -sha256 -key valkey.key -subj '/CN=Valkey Server Test Cert' | openssl x509 -req -sha256 -CA ca.crt -CAkey ca.key -CAserial ca.txt -CAcreateserial -days 365 -out valkey.crt
    COMMAND openssl genrsa -out client.key 2048
    COMMAND openssl req -new -sha256 -key client.key -subj '/CN=Valkey Client Test Cert' | openssl x509 -req -sha256 -CA ca.crt -CAkey ca.key -CAserial ca.txt -CAcreateserial -days 365 -out client.crt
    )
  add_custom_target(generate_tls_configs DEPENDS ${TLS_CONFIGS})
  set(TLS_LIBRARY valkey_tls)
endif()

if(ENABLE_IPV6_TESTS)
  set(NO_IPV6 "")
else()
  set(NO_IPV6 "true") # Ignore command
endif()

# Targets to setup clusters for testing
if(TEST_WITH_REDIS_VERSION)
  set(CLUSTER_SCRIPT "${CMAKE_SOURCE_DIR}/tests/scripts/redis-cluster")
  set(CLUSTER_VERSION "REDIS_VERSION=${TEST_WITH_REDIS_VERSION}")
else()
  set(CLUSTER_SCRIPT "${CMAKE_SOURCE_DIR}/tests/scripts/valkey-cluster")
  set(CLUSTER_VERSION "VALKEY_VERSION=${TEST_WITH_VALKEY_VERSION}")
endif()
add_custom_target(start
  COMMAND PORT=7000 ${CLUSTER_VERSION} ${CLUSTER_SCRIPT} start
  COMMAND PORT=7100 ${CLUSTER_VERSION} ADDITIONAL_OPTIONS='--requirepass secretword --masterauth secretword' ADDITIONAL_CLI_OPTIONS='-a secretword' ${CLUSTER_SCRIPT} start
  COMMAND ${NO_IPV6} PORT=7200 ${CLUSTER_VERSION} CLUSTER_HOST=::1 ADDITIONAL_OPTIONS='--bind ::1' ADDITIONAL_CLI_OPTIONS='-h ::1' ${CLUSTER_SCRIPT} start
)
add_custom_target(stop
  COMMAND PORT=7000 ${CLUSTER_SCRIPT} stop
  COMMAND PORT=7100 ${CLUSTER_SCRIPT} stop
  COMMAND ${NO_IPV6} PORT=7200 ${CLUSTER_SCRIPT} stop
)

# Find dependencies
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(GLIB_LIBRARY IMPORTED_TARGET glib-2.0)
endif()
find_library(LIBUV_LIBRARY uv HINTS /usr/lib/x86_64-linux-gnu)
find_path(LIBUV_INCLUDE_DIR uv.h)
find_library(LIBEV_LIBRARY ev HINTS /usr/lib/x86_64-linux-gnu)
find_path(LIBEV_INCLUDE_DIR ev.h)
find_library(LIBEVENT_LIBRARY event HINTS /usr/lib/x86_64-linux-gnu)
find_path(LIBEVENT_INCLUDE_DIR event2/event.h)
if(LIBEVENT_INCLUDE_DIR)
  include_directories(${LIBEVENT_INCLUDE_DIR})
endif()
if(MSVC OR MINGW)
  find_library(LIBEVENT_LIBRARY Libevent)
else()
  # Undefine any -DNDEBUG to make sure that tests can assert.
  # CMake defines NDEBUG in Release and RelWithDebInfo.
  add_compile_options("-UNDEBUG")
endif()

# Add the non-cluster test binary.
# Some tests are unit tests which require us to include private headers.
add_executable(client_test client_test.c)
target_include_directories(client_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(client_test valkey_unittest)
add_test(NAME client_test COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/test.sh")
if(ENABLE_TLS)
  target_compile_definitions(client_test PUBLIC VALKEY_TEST_TLS=1)
  set_property(TEST client_test APPEND PROPERTY ENVIRONMENT "TEST_TLS=1")
endif()
if(ENABLE_RDMA)
  target_compile_definitions(client_test PUBLIC VALKEY_TEST_RDMA=1)
  set_property(TEST client_test APPEND PROPERTY ENVIRONMENT "TEST_RDMA=1")
endif()
if(LIBEVENT_LIBRARY)
  target_compile_definitions(client_test PUBLIC VALKEY_TEST_ASYNC=1)
  target_link_libraries(client_test ${LIBEVENT_LIBRARY})
  set_property(TEST client_test APPEND PROPERTY ENVIRONMENT "TEST_CLUSTER=1")
endif()
if(TEST_WITH_REDIS_VERSION)
  set_property(TEST client_test APPEND PROPERTY ENVIRONMENT "VALKEY_SERVER=redis-server")
endif()

# Unit tests
add_executable(ut_parse_cmd ut_parse_cmd.c test_utils.c)
target_include_directories(ut_parse_cmd PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(ut_parse_cmd valkey_unittest)
add_test(NAME ut_parse_cmd COMMAND "$<TARGET_FILE:ut_parse_cmd>")

add_executable(ut_slotmap_update ut_slotmap_update.c)
target_include_directories(ut_slotmap_update PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(ut_slotmap_update valkey_unittest)
add_test(NAME ut_slotmap_update COMMAND "$<TARGET_FILE:ut_slotmap_update>")

# Cluster tests
add_executable(ct_commands ct_commands.c test_utils.c)
target_link_libraries(ct_commands valkey ${TLS_LIBRARY})
add_test(NAME ct_commands COMMAND "$<TARGET_FILE:ct_commands>")

if (LIBEVENT_LIBRARY)
  add_executable(ct_async ct_async.c)
  target_link_libraries(ct_async valkey ${TLS_LIBRARY} ${LIBEVENT_LIBRARY})
  add_test(NAME ct_async COMMAND "$<TARGET_FILE:ct_async>")

  add_executable(ct_connection ct_connection.c test_utils.c)
  target_link_libraries(ct_connection valkey ${TLS_LIBRARY} ${LIBEVENT_LIBRARY})
  add_test(NAME ct_connection COMMAND "$<TARGET_FILE:ct_connection>")

  add_executable(ct_pipeline ct_pipeline.c)
  target_link_libraries(ct_pipeline valkey ${TLS_LIBRARY} ${LIBEVENT_LIBRARY})
  add_test(NAME ct_pipeline COMMAND "$<TARGET_FILE:ct_pipeline>")

  add_executable(ct_connection_ipv6 ct_connection_ipv6.c)
  target_link_libraries(ct_connection_ipv6 valkey ${TLS_LIBRARY} ${LIBEVENT_LIBRARY})
  add_test(NAME ct_connection_ipv6 COMMAND "$<TARGET_FILE:ct_connection_ipv6>")
  if(NOT ENABLE_IPV6_TESTS)
    set_tests_properties(ct_connection_ipv6 PROPERTIES DISABLED True)
  endif()

  add_executable(ct_out_of_memory_handling ct_out_of_memory_handling.c)
  target_link_libraries(ct_out_of_memory_handling valkey ${TLS_LIBRARY} ${LIBEVENT_LIBRARY})
  add_test(NAME ct_out_of_memory_handling COMMAND "$<TARGET_FILE:ct_out_of_memory_handling>")

  add_executable(ct_specific_nodes ct_specific_nodes.c test_utils.c)
  target_link_libraries(ct_specific_nodes valkey ${TLS_LIBRARY} ${LIBEVENT_LIBRARY})
  add_test(NAME ct_specific_nodes COMMAND "$<TARGET_FILE:ct_specific_nodes>")

  # Tests using simulated Valkey node
  add_executable(clusterclient clusterclient.c)
  target_link_libraries(clusterclient valkey ${TLS_LIBRARY})
  add_executable(clusterclient_async clusterclient_async.c)
  target_link_libraries(clusterclient_async valkey ${TLS_LIBRARY} ${LIBEVENT_LIBRARY})
  add_test(NAME set-get-test
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/set-get-test.sh"
                   "$<TARGET_FILE:clusterclient>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME set-get-test-async
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/set-get-test.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME ask-redirect-test
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/ask-redirect-test.sh"
                    "$<TARGET_FILE:clusterclient>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME ask-redirect-test-async
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/ask-redirect-test.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME ask-redirect-using-cluster-nodes-test-async
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/ask-redirect-using-cluster-nodes-test.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME moved-redirect-test
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/moved-redirect-test.sh"
                   "$<TARGET_FILE:clusterclient>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME moved-redirect-test-async
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/moved-redirect-test.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME moved-redirect-using-cluster-nodes-test-async
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/moved-redirect-using-cluster-nodes-test.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME dbsize-to-all-nodes-test
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/dbsize-to-all-nodes-test.sh"
                   "$<TARGET_FILE:clusterclient>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME dbsize-to-all-nodes-test-async
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/dbsize-to-all-nodes-test.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME dbsize-to-all-nodes-during-scaledown-test
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/dbsize-to-all-nodes-during-scaledown-test.sh"
                   "$<TARGET_FILE:clusterclient>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME dbsize-to-all-nodes-during-scaledown-test-async
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/dbsize-to-all-nodes-during-scaledown-test-async.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME timeout-handling-test
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/timeout-handling-test.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME connect-error-using-cluster-nodes-test
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/connect-error-using-cluster-nodes-test.sh"
                   "$<TARGET_FILE:clusterclient>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME command-from-callback-test
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/command-from-callback-test.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME ask-redirect-connection-error-test
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/ask-redirect-connection-error-test.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME cluster-down-test
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/cluster-down-test.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME connection-error-test
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/connection-error-test.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME redirect-with-hostname-test
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/redirect-with-hostname-test.sh"
                   "$<TARGET_FILE:clusterclient>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME redirect-with-ipv6-test
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/redirect-with-ipv6-test.sh"
                   "$<TARGET_FILE:clusterclient>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME redirect-with-ipv6-async-test
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/redirect-with-ipv6-test.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  if(NOT ENABLE_IPV6_TESTS)
    set_tests_properties(redirect-with-ipv6-test PROPERTIES DISABLED True)
    set_tests_properties(redirect-with-ipv6-async-test PROPERTIES DISABLED True)
  endif()
  add_test(NAME redirect-with-hostname-test-async
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/redirect-with-hostname-test.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME cluster-scale-down-test
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/cluster-scale-down-test.sh"
                  "$<TARGET_FILE:clusterclient>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME slots-not-served-test
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/slots-not-served-test.sh"
                   "$<TARGET_FILE:clusterclient>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME slots-not-served-test-async
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/slots-not-served-test-async.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME client-disconnect-test-async
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/client-disconnect-test.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME client-disconnect-without-slotmap-update-test-async
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/client-disconnect-without-slotmap-update-test.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME connect-during-cluster-startup-test-async
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/connect-during-cluster-startup-test.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
  add_test(NAME connect-during-cluster-startup-using-cluster-nodes-test-async
           COMMAND "${CMAKE_SOURCE_DIR}/tests/scripts/connect-during-cluster-startup-using-cluster-nodes-test.sh"
                   "$<TARGET_FILE:clusterclient_async>"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/scripts/")
endif()

if(LIBUV_LIBRARY AND LIBUV_INCLUDE_DIR)
  add_executable(ct_async_libuv ct_async_libuv.c)
  target_include_directories(ct_async_libuv PRIVATE ${LIBUV_INCLUDE_DIR})
  target_link_libraries(ct_async_libuv valkey ${TLS_LIBRARY} ${LIBUV_LIBRARY})
  add_test(NAME ct_async_libuv COMMAND "$<TARGET_FILE:ct_async_libuv>")
else()
  add_test(NAME ct_async_libuv COMMAND "")
  set_tests_properties(ct_async_libuv PROPERTIES DISABLED True)
endif()

if(LIBEV_LIBRARY AND LIBEV_INCLUDE_DIR)
  add_executable(ct_async_libev ct_async_libev.c)
  target_include_directories(ct_async_libev PRIVATE ${LIBEV_INCLUDE_DIR})
  target_link_libraries(ct_async_libev valkey ${TLS_LIBRARY} ${LIBEV_LIBRARY})
  add_test(NAME ct_async_libev COMMAND "$<TARGET_FILE:ct_async_libev>")
else()
  add_test(NAME ct_async_libev COMMAND "")
  set_tests_properties(ct_async_libev PROPERTIES DISABLED True)
endif()

if(GLIB_LIBRARY_FOUND)
  add_executable(ct_async_glib ct_async_glib.c)
  target_link_libraries(ct_async_glib valkey ${TLS_LIBRARY} PkgConfig::GLIB_LIBRARY)
  add_test(NAME ct_async_glib COMMAND "$<TARGET_FILE:ct_async_glib>")
else()
  add_test(NAME ct_async_glib COMMAND "")
  set_tests_properties(ct_async_glib PROPERTIES DISABLED True)
endif()
