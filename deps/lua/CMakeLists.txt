project(lualib)

# Build lua library
set(LUA_INSTALL_DIR ${CMAKE_BINARY_DIR}/lua-build)
set(LUA_SRC_DIR ${CMAKE_CURRENT_LIST_DIR})
if (NOT EXISTS ${LUA_INSTALL_DIR}/lib/liblua.a)
    message(STATUS "Building lua (custom build)")
    execute_process(COMMAND mkdir -p "${LUA_INSTALL_DIR}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND make -j${VALKEY_PROCESSOR_COUNT} all WORKING_DIRECTORY "${LUA_SRC_DIR}/src"
                                                                                   COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND make -j${VALKEY_PROCESSOR_COUNT} install INSTALL_TOP=${LUA_INSTALL_DIR}
                    WORKING_DIRECTORY "${LUA_SRC_DIR}" COMMAND_ERROR_IS_FATAL ANY)
endif ()

# Import the compiled library as a CMake target "liblua"
add_library(lualib STATIC IMPORTED GLOBAL)
set_target_properties(lualib PROPERTIES IMPORTED_LOCATION "${LUA_INSTALL_DIR}/lib/liblua.a"
                                        INCLUDE_DIRECTORIES "${LUA_INSTALL_DIR}/include")

message(STATUS "lualib import from ${LUA_INSTALL_DIR}/lib/liblua.a")
