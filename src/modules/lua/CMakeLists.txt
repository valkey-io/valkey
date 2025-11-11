project(luaengine)

option(WITHOUT_LUA "Build Valkey without Lua scripting engine")

if (VALKEY_DEBUG_BUILD)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -W -Wall -fno-common -g -ggdb -std=c99 -O2 -D_GNU_SOURCE")
else ()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -W -Wall -fno-common -O3 -std=c99  -D_GNU_SOURCE")
endif()

set(LUA_ENGINE_SRCS
    engine_lua.c
    script_lua.c
    function_lua.c
    debug_lua.c
    list.c)

add_library(luaengine SHARED "${LUA_ENGINE_SRCS}")

add_dependencies(luaengine lualib)
target_link_libraries(luaengine PRIVATE lualib)
target_include_directories(luaengine PRIVATE ../../../deps/lua/src)

install(TARGETS luaengine
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
