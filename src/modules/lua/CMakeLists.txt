project(valkeylua)

if (VALKEY_DEBUG_BUILD)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -W -Wall -fno-common -g -ggdb -std=c99 -O2 -D_GNU_SOURCE")
else ()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -W -Wall -fno-common -O3 -std=c99  -D_GNU_SOURCE")
endif ()

set(LUA_ENGINE_SRCS
    engine_lua.c
    script_lua.c
    function_lua.c
    debug_lua.c
    list.c)

if (BUILD_LUA STREQUAL "module")
    list(APPEND LUA_ENGINE_SRCS ../../sha1.c)
    list(APPEND LUA_ENGINE_SRCS ../../rand.c)
endif ()

if (BUILD_LUA STREQUAL "static")
    message(STATUS "Building STATIC LUA module")
    add_library(valkeylua STATIC "${LUA_ENGINE_SRCS}")
    target_link_libraries(valkeylua PUBLIC lualib fpconv)
    target_include_directories(valkeylua PUBLIC ../../../deps/lua/src)
else ()
    message(STATUS "Building DYNAMIC LUA module")
    add_library(valkeylua SHARED "${LUA_ENGINE_SRCS}")
    target_link_libraries(valkeylua PRIVATE lualib fpconv)
    target_include_directories(valkeylua PRIVATE ../../../deps/lua/src)
endif ()

install(TARGETS valkeylua LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
