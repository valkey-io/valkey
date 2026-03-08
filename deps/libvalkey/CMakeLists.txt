cmake_minimum_required(VERSION 3.7.0)

macro(getDefinedVersion name)
  set(VERSION_REGEX "^#define LIBVALKEY_${name} (.+)$")
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/valkey/valkey.h"
    MATCHED_LINE REGEX ${VERSION_REGEX})
  string(REGEX REPLACE ${VERSION_REGEX} "\\1" ${name} "${MATCHED_LINE}")
endmacro()

getDefinedVersion(VERSION_MAJOR)
getDefinedVersion(VERSION_MINOR)
getDefinedVersion(VERSION_PATCH)
set(VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
message("Detected libvalkey version: ${VERSION}")

project(libvalkey LANGUAGES "C" VERSION "${VERSION}")
INCLUDE(GNUInstallDirs)

option(ENABLE_THREADS "Enable thread-safe initialization" ON)
OPTION(BUILD_SHARED_LIBS "Build shared libraries" ON)
OPTION(ENABLE_TLS "Build valkey_tls for TLS support" OFF)
OPTION(DISABLE_TESTS "If tests should be compiled or not" OFF)
OPTION(ENABLE_EXAMPLES "Enable building valkey examples" OFF)
option(ENABLE_IPV6_TESTS "Enable IPv6 tests requiring special prerequisites" OFF)
OPTION(ENABLE_RDMA "Build valkey_rdma for RDMA support" OFF)
OPTION(ENABLE_DLOPEN_RDMA "Build valkey_rdma with dynamic loading" OFF)

# Libvalkey requires C99 (-std=c99)
SET(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS OFF)
SET(CMAKE_DEBUG_POSTFIX d)

# Set target-common flags
if(NOT WIN32)
  add_compile_options(-Werror -Wall -Wextra -pedantic)
  add_compile_options(-Wstrict-prototypes -Wwrite-strings -Wno-missing-field-initializers)
else()
  add_definitions(-D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN)
endif()

set(valkey_sources
    src/adlist.c
    src/alloc.c
    src/async.c
    src/cluster.c
    src/command.c
    src/conn.c
    src/crc16.c
    src/net.c
    src/read.c
    src/sockcompat.c
    src/valkey.c
    src/vkutil.c)

# Allow the libvalkey provided sds and dict types to be replaced by
# compatible implementations (like Valkey's).
# A replaced type is not included in a built archive or shared library.
if(NOT DICT_INCLUDE_DIR)
  set(valkey_sources ${valkey_sources} src/dict.c)
  set(DICT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()
if(NOT SDS_INCLUDE_DIR)
  set(valkey_sources ${valkey_sources} src/sds.c)
  set(SDS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

ADD_LIBRARY(valkey ${valkey_sources})
ADD_LIBRARY(valkey::valkey ALIAS valkey)
set_target_properties(valkey PROPERTIES
  C_VISIBILITY_PRESET hidden
  WINDOWS_EXPORT_ALL_SYMBOLS TRUE
  SOVERSION "${VERSION_MAJOR}"
  VERSION "${VERSION}")

if(MSVC)
  # Produce object files that contain debug info.
  target_compile_options(valkey PRIVATE /Z7)
endif()

set(valkey_link_libraries)
set(valkey_compile_definitions)

IF(WIN32)
    list(APPEND valkey_link_libraries ws2_32 crypt32)
ELSEIF(CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
    list(APPEND valkey_link_libraries m)
ELSEIF(CMAKE_SYSTEM_NAME MATCHES "SunOS")
    list(APPEND valkey_link_libraries socket)
ENDIF()

if(ENABLE_THREADS)
    find_package(Threads REQUIRED)
    list(APPEND valkey_link_libraries Threads::Threads)
    list(APPEND valkey_compile_definitions VALKEY_USE_THREADS)
endif()

if(valkey_link_libraries)
    target_link_libraries(valkey PUBLIC ${valkey_link_libraries})
endif()

if(valkey_compile_definitions)
    target_compile_definitions(valkey PRIVATE ${valkey_compile_definitions})
endif()

TARGET_INCLUDE_DIRECTORIES(valkey
    PUBLIC
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/valkey>
    PRIVATE
        $<BUILD_INTERFACE:${DICT_INCLUDE_DIR}>
        $<BUILD_INTERFACE:${SDS_INCLUDE_DIR}>
)

CONFIGURE_FILE(valkey.pc.in valkey.pc @ONLY)

set(CPACK_PACKAGE_VENDOR "Valkey")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Minimalistic C client library for Valkey")
set(CPACK_PACKAGE_DESCRIPTION "\
Libvalkey is a minimalistic C client library for the Valkey, KeyDB, and Redis databases.

It is minimalistic because it just adds minimal support for the protocol, \
but at the same time it uses a high level printf-alike API in order to make \
it much higher level than otherwise suggested by its minimal code base and the \
lack of explicit bindings for every server command.

Apart from supporting sending commands and receiving replies, it comes with a \
reply parser that is decoupled from the I/O layer. It is a stream parser designed \
for easy reusability, which can for instance be used in higher level language bindings \
for efficient reply parsing.

Libvalkey only supports the binary-safe RESP protocol, so you can use it with any Redis \
compatible server >= 1.2.0.

The library comes with multiple APIs. There is the synchronous API, the asynchronous API \
and the reply parsing API.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/valkey-io/libvalkey")
set(CPACK_PACKAGE_CONTACT "michael dot grunder at gmail dot com")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
set(CPACK_RPM_PACKAGE_GROUP "Productivity/Databases/Clients")
set(CPACK_RPM_PACKAGE_LICENSE "BSD-3-Clause")
include(CPack)

INSTALL(TARGETS valkey
    EXPORT valkey-targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

if(MSVC AND BUILD_SHARED_LIBS)
  # Install linker generated program database file.
  install(FILES $<TARGET_PDB_FILE:valkey>
          DESTINATION ${CMAKE_INSTALL_BINDIR}
          CONFIGURATIONS Debug RelWithDebInfo)
endif()

# Install public headers
install(DIRECTORY include/valkey
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        PATTERN "tls.h" EXCLUDE
        PATTERN "rdma.h" EXCLUDE
        PATTERN "adapters/macosx.h" EXCLUDE)
if(APPLE)
  install(FILES include/valkey/adapters/macosx.h
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/valkey/adapters)
endif()

INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/valkey.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

export(EXPORT valkey-targets
    FILE "${CMAKE_CURRENT_BINARY_DIR}/valkey-targets.cmake"
    NAMESPACE valkey::)

if(WIN32)
    SET(CMAKE_CONF_INSTALL_DIR share/valkey)
else()
    SET(CMAKE_CONF_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/valkey)
endif()
SET(INCLUDE_INSTALL_DIR include)
include(CMakePackageConfigHelpers)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/valkey-config-version.cmake"
                                 COMPATIBILITY SameMajorVersion)
configure_package_config_file(valkey-config.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/valkey-config.cmake
                              INSTALL_DESTINATION ${CMAKE_CONF_INSTALL_DIR}
                              PATH_VARS INCLUDE_INSTALL_DIR)

INSTALL(EXPORT valkey-targets
        FILE valkey-targets.cmake
        NAMESPACE valkey::
        DESTINATION ${CMAKE_CONF_INSTALL_DIR})

INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/valkey-config.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/valkey-config-version.cmake
        DESTINATION ${CMAKE_CONF_INSTALL_DIR})


IF(ENABLE_TLS)
    IF (NOT OPENSSL_ROOT_DIR)
        IF (APPLE)
            SET(OPENSSL_ROOT_DIR "/usr/local/opt/openssl")
        ENDIF()
    ENDIF()
    FIND_PACKAGE(OpenSSL REQUIRED)
    SET(valkey_tls_sources
        src/tls.c)
    ADD_LIBRARY(valkey_tls ${valkey_tls_sources})
    ADD_LIBRARY(valkey::valkey_tls ALIAS valkey_tls)

    TARGET_INCLUDE_DIRECTORIES(valkey_tls
        PRIVATE
            $<INSTALL_INTERFACE:include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/valkey>
            $<BUILD_INTERFACE:${DICT_INCLUDE_DIR}>
            $<BUILD_INTERFACE:${SDS_INCLUDE_DIR}>
    )

    set_target_properties(valkey_tls PROPERTIES
      C_VISIBILITY_PRESET hidden
      WINDOWS_EXPORT_ALL_SYMBOLS TRUE
      SOVERSION "${VERSION_MAJOR}"
      VERSION "${VERSION}")
    if(MSVC)
      # Produce object files that contain debug info.
      target_compile_options(valkey_tls PRIVATE /Z7)
    endif()
    target_link_libraries(valkey_tls PRIVATE valkey::valkey OpenSSL::SSL)
    CONFIGURE_FILE(valkey_tls.pc.in valkey_tls.pc @ONLY)

    INSTALL(TARGETS valkey_tls
        EXPORT valkey_tls-targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    # Install public header
    install(FILES include/valkey/tls.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/valkey)

    if(MSVC AND BUILD_SHARED_LIBS)
      # Install linker generated program database file.
      install(FILES $<TARGET_PDB_FILE:valkey_tls>
              DESTINATION ${CMAKE_INSTALL_BINDIR}
              CONFIGURATIONS Debug RelWithDebInfo)
    endif()

    INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/valkey_tls.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

    export(EXPORT valkey_tls-targets
           FILE "${CMAKE_CURRENT_BINARY_DIR}/valkey_tls-targets.cmake"
           NAMESPACE valkey::)

    if(WIN32)
        SET(CMAKE_CONF_INSTALL_DIR share/valkey_tls)
    else()
        SET(CMAKE_CONF_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/valkey_tls)
    endif()
    configure_package_config_file(valkey_tls-config.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/valkey_tls-config.cmake
                                  INSTALL_DESTINATION ${CMAKE_CONF_INSTALL_DIR}
                                  PATH_VARS INCLUDE_INSTALL_DIR)

    INSTALL(EXPORT valkey_tls-targets
        FILE valkey_tls-targets.cmake
        NAMESPACE valkey::
        DESTINATION ${CMAKE_CONF_INSTALL_DIR})

    INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/valkey_tls-config.cmake
        DESTINATION ${CMAKE_CONF_INSTALL_DIR})
ENDIF()

if(ENABLE_RDMA)
    set(valkey_rdma_sources src/rdma.c)
    add_library(valkey_rdma ${valkey_rdma_sources})
    add_library(valkey::valkey_rdma ALIAS valkey_rdma)
    
    if (ENABLE_DLOPEN_RDMA)
        message(STATUS "libvalkey: Building RDMA with dynamic loading (dlopen)")
        target_compile_definitions(valkey_rdma PRIVATE DLOPEN_RDMA)
    else()
        message(STATUS "libvalkey: Building RDMA with static/hard linking")
        find_library(RDMACM_LIBRARIES rdmacm REQUIRED)
        find_library(IBVERBS_LIBRARIES ibverbs REQUIRED)
        target_link_libraries(valkey_rdma LINK_PRIVATE ${RDMACM_LIBRARIES} ${IBVERBS_LIBRARIES})
    endif()

    target_include_directories(valkey_rdma
        PRIVATE
            $<INSTALL_INTERFACE:include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/valkey>
            $<BUILD_INTERFACE:${DICT_INCLUDE_DIR}>
            $<BUILD_INTERFACE:${SDS_INCLUDE_DIR}>
    )

    set_target_properties(valkey_rdma PROPERTIES
      C_VISIBILITY_PRESET hidden
      WINDOWS_EXPORT_ALL_SYMBOLS TRUE
      SOVERSION "${VERSION_MAJOR}"
      VERSION "${VERSION}")
    configure_file(valkey_rdma.pc.in valkey_rdma.pc @ONLY)

    install(TARGETS valkey_rdma
        EXPORT valkey_rdma-targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    # Install public header
    install(FILES include/valkey/rdma.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/valkey)

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/valkey_rdma.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

    export(EXPORT valkey_rdma-targets
           FILE "${CMAKE_CURRENT_BINARY_DIR}/valkey_rdma-targets.cmake"
           NAMESPACE valkey::)

    set(CMAKE_CONF_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/valkey_rdma)
    configure_package_config_file(valkey_rdma-config.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/valkey_rdma-config.cmake
                                  INSTALL_DESTINATION ${CMAKE_CONF_INSTALL_DIR}
                                  PATH_VARS INCLUDE_INSTALL_DIR)

    install(EXPORT valkey_rdma-targets
        FILE valkey_rdma-targets.cmake
        NAMESPACE valkey::
        DESTINATION ${CMAKE_CONF_INSTALL_DIR})

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/valkey_rdma-config.cmake
        DESTINATION ${CMAKE_CONF_INSTALL_DIR})
endif()

# Add tests
if(NOT DISABLE_TESTS)
  # Unit tests uses a static library to ensure all symbols are visible.
  # This single library also bundles TLS and RDMA when enabled.
  add_library(valkey_unittest STATIC ${valkey_sources} ${valkey_tls_sources} ${valkey_rdma_sources})

  # Mirror the include directories.
  get_target_property(include_directories valkey::valkey INCLUDE_DIRECTORIES)
  target_include_directories(valkey_unittest PUBLIC ${include_directories})

  if(valkey_compile_definitions)
    target_compile_definitions(valkey_unittest PRIVATE ${valkey_compile_definitions})
  endif()
  if(valkey_link_libraries)
    target_link_libraries(valkey_unittest PUBLIC ${valkey_link_libraries})
  endif()
  if(ENABLE_TLS)
    target_link_libraries(valkey_unittest PRIVATE OpenSSL::SSL)
  endif()
  if(ENABLE_RDMA)
    target_link_libraries(valkey_unittest PRIVATE ${RDMACM_LIBRARIES} ${IBVERBS_LIBRARIES})
  endif()

  # Create libvalkey_unittest.a in the tests directory.
  set_target_properties(valkey_unittest PROPERTIES
                        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")

  # Make sure ctest prints the output when a test fails.
  set(CMAKE_CTEST_ARGUMENTS "--output-on-failure")
  include(CTest)
  add_subdirectory(tests)
endif()

# Add examples
IF(ENABLE_EXAMPLES)
    ADD_SUBDIRECTORY(examples)
ENDIF(ENABLE_EXAMPLES)
