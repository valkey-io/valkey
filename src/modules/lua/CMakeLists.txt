project(valkeylua)

if (VALKEY_DEBUG_BUILD)
    if (MSVC)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W3 /Z7 /Od -D_GNU_SOURCE")
    else()
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -W -Wall -fno-common -g -ggdb -std=c99 -O2 -D_GNU_SOURCE")
    endif()
else ()
    if (MSVC)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W3 /O2 -D_GNU_SOURCE")
    else()
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -W -Wall -fno-common -O3 -std=c99  -D_GNU_SOURCE")
    endif()
endif()

set(LUA_ENGINE_SRCS
    ${CMAKE_CURRENT_BINARY_DIR}/patched_engine_lua.c
    ${CMAKE_CURRENT_BINARY_DIR}/patched_script_lua.c
    ${CMAKE_CURRENT_BINARY_DIR}/patched_function_lua.c
    ${CMAKE_CURRENT_BINARY_DIR}/patched_debug_lua.c
    ${CMAKE_CURRENT_BINARY_DIR}/patched_list.c
    ../../sha1.c
    ../../rand.c)

foreach(F engine_lua.c script_lua.c function_lua.c debug_lua.c list.c)
    file(READ ${F} CONTENT)
    string(REPLACE "../../valkeymodule.h" "valkeymodule.h" CONTENT "${CONTENT}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/patched_${F}" "${CONTENT}")
endforeach()

add_library(valkeylua SHARED "${LUA_ENGINE_SRCS}")

if(MSVC)
  set_property(TARGET valkeylua APPEND PROPERTY LINK_FLAGS "/EXPORT:ValkeyModule_OnLoad")
endif()

target_link_libraries(valkeylua PRIVATE lualib fpconv)
target_include_directories(valkeylua PRIVATE ../../../deps/lua/src)

install(TARGETS valkeylua
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
