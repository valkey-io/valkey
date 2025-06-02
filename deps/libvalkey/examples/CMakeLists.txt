cmake_minimum_required(VERSION 3.7.0)
project(examples LANGUAGES C)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  # This CMakeLists.txt is used standalone to build the examples.
  # Find required valkey package to get include paths.
  find_package(valkey REQUIRED)
  find_package(valkey_tls QUIET)
else()
  # This CMakeLists.txt is included, most likely from libvalkey's project
  # CMakeLists.txt. Add path to enable <valkey/x.h> includes.
  include_directories(${CMAKE_SOURCE_DIR}/include)
endif()

# Check for GLib
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(GLIB2 glib-2.0)
  if(GLIB2_FOUND)
    add_executable(example-async-glib async-glib.c)
    target_include_directories(example-async-glib PUBLIC ${GLIB2_INCLUDE_DIRS})
    target_link_libraries(example-async-glib valkey::valkey ${GLIB2_LIBRARIES})
  endif()
endif()

find_path(LIBEV ev.h
  HINTS /usr/local /usr/opt/local
  ENV LIBEV_INCLUDE_DIR)
if(LIBEV)
  add_executable(example-async-libev async-libev.c)
  target_link_libraries(example-async-libev valkey::valkey ev)
endif()

find_path(LIBEVENT event.h)
if(LIBEVENT)
  add_executable(example-async-libevent async-libevent.c)
  target_link_libraries(example-async-libevent valkey::valkey event)

  if(ENABLE_TLS)
    add_executable(example-async-libevent-tls async-libevent-tls.c)
    target_link_libraries(example-async-libevent-tls valkey::valkey valkey::valkey_tls event)
  endif()
endif()

find_path(LIBHV hv/hv.h)
if(LIBHV)
  add_executable(example-async-libhv async-libhv.c)
  target_link_libraries(example-async-libhv valkey::valkey hv)
endif()

find_path(LIBUV uv.h)
if(LIBUV)
  add_executable(example-async-libuv async-libuv.c)
  target_link_libraries(example-async-libuv valkey::valkey uv)
endif()

find_path(LIBSDEVENT systemd/sd-event.h)
if(LIBSDEVENT)
  add_executable(example-async-libsdevent async-libsdevent.c)
  target_link_libraries(example-async-libsdevent valkey::valkey systemd)
endif()

if(APPLE)
  find_library(CF CoreFoundation)
  add_executable(example-async-macosx async-macosx.c)
  target_link_libraries(example-async-macosx valkey::valkey ${CF})
endif()

if(ENABLE_TLS)
  add_executable(example-blocking-tls blocking-tls.c)
  target_link_libraries(example-blocking-tls valkey::valkey valkey::valkey_tls)
endif()

add_executable(example-blocking blocking.c)
target_link_libraries(example-blocking valkey::valkey)

add_executable(example-blocking-push blocking-push.c)
target_link_libraries(example-blocking-push valkey::valkey)

if(LIBEVENT)
  add_executable(example-cluster-async cluster-async.c)
  target_link_libraries(example-cluster-async valkey::valkey event)

  if(ENABLE_TLS)
    add_executable(example-cluster-async-tls cluster-async-tls.c)
    target_link_libraries(example-cluster-async-tls valkey::valkey valkey::valkey_tls event)
  endif()

  add_executable(example-cluster-clientside-caching-async cluster-clientside-caching-async.c)
  target_link_libraries(example-cluster-clientside-caching-async valkey::valkey event)
endif()

add_executable(example-cluster-simple cluster-simple.c)
target_link_libraries(example-cluster-simple valkey::valkey)

if(ENABLE_TLS)
  add_executable(example-cluster-tls cluster-tls.c)
  target_link_libraries(example-cluster-tls valkey::valkey valkey::valkey_tls)
endif()
