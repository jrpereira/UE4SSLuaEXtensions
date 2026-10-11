cmake_minimum_required(VERSION 3.22)

get_filename_component(UE4SSLFB_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${UE4SSLFB_REPO_ROOT}/shared/bootstrap/cmake/BridgeBootstrap.cmake")
bridge_read_product_version(UE4SSLFB_PRODUCT_VERSION "${CMAKE_CURRENT_LIST_DIR}/contract/Version.hpp")
project(UE4SSLuaFileBridgeImplementation VERSION ${UE4SSLFB_PRODUCT_VERSION} LANGUAGES CXX)

option(UE4SSLEB_BUILD_TESTS "Build portable unit tests" ON)
if(UE4SSLEB_BUILD_TESTS AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
endif()
if(NOT TARGET UE4SSXBDispatch)
    add_subdirectory("${UE4SSLFB_REPO_ROOT}/shared/dispatch" shared-dispatch)
endif()

# Descriptor magic 'LFB1': the bootstrap and the implementation must agree, and
# FileBridgeMod.cpp checks it against NativeContract::implementation_magic.
set(UE4SSLFB_MAGIC 0x4C464231)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/main.json.in" "${CMAKE_CURRENT_BINARY_DIR}/main.json" @ONLY)
add_bridge_bootstrap(
    TARGET UE4SSLFBBootstrap
    PRODUCT UE4SSLuaFileBridge
    DISPLAY_NAME "UE4SS Lua File Bridge"
    MAGIC ${UE4SSLFB_MAGIC}
    CLAIM_NAME UE4SSLFB
    VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/contract/Version.hpp"
    DIST_FOLDER 0_ModCore_UE4SSLuaFileBridge
    SELECTOR_FILE "${CMAKE_CURRENT_BINARY_DIR}/main.json"
    ENABLED_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/enabled.txt"
    SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/lua/main_files.lua"
    SETTINGS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/mod_settings.ini"
)

# The Lua layer is embedded as fixed-size raw-string chunks: MSVC limits one
# string literal to about 16 KB.
set(UE4SSLFB_LUA_API_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/lua/files_api.lua")
set(UE4SSLFB_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(UE4SSLFB_GENERATED_LUA_API "${UE4SSLFB_GENERATED_INCLUDE_DIR}/EmbeddedLuaAPI.hpp")
file(MAKE_DIRECTORY "${UE4SSLFB_GENERATED_INCLUDE_DIR}")
file(READ "${UE4SSLFB_LUA_API_SOURCE}" UE4SSLFB_LUA_API)
set(UE4SSLFB_LUA_API_CHUNK_SIZE 7000)
string(LENGTH "${UE4SSLFB_LUA_API}" UE4SSLFB_LUA_API_LENGTH)
string(FIND "${UE4SSLFB_LUA_API}" ")UE4SSLFB_LUA" UE4SSLFB_LUA_API_DELIMITER)
if(NOT UE4SSLFB_LUA_API_DELIMITER EQUAL -1)
    message(FATAL_ERROR "files_api.lua contains the raw-string delimiter )UE4SSLFB_LUA")
endif()
set(UE4SSLFB_LUA_API_CHUNKS "")
set(UE4SSLFB_LUA_API_CHUNK_COUNT 0)
set(UE4SSLFB_LUA_API_OFFSET 0)
while(UE4SSLFB_LUA_API_OFFSET LESS UE4SSLFB_LUA_API_LENGTH)
    string(SUBSTRING "${UE4SSLFB_LUA_API}" ${UE4SSLFB_LUA_API_OFFSET} ${UE4SSLFB_LUA_API_CHUNK_SIZE}
        UE4SSLFB_LUA_API_CHUNK)
    string(APPEND UE4SSLFB_LUA_API_CHUNKS "    R\"UE4SSLFB_LUA(${UE4SSLFB_LUA_API_CHUNK})UE4SSLFB_LUA\",\n")
    math(EXPR UE4SSLFB_LUA_API_OFFSET "${UE4SSLFB_LUA_API_OFFSET} + ${UE4SSLFB_LUA_API_CHUNK_SIZE}")
    math(EXPR UE4SSLFB_LUA_API_CHUNK_COUNT "${UE4SSLFB_LUA_API_CHUNK_COUNT} + 1")
endwhile()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbeddedLuaAPI.hpp.in" "${UE4SSLFB_GENERATED_LUA_API}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${UE4SSLFB_LUA_API_SOURCE}")

if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR "The ABI-pinned UE4SS target must be built with MSVC.")
    endif()
    enable_language(RC)
    set(UE4SSLFB_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/Version.rc")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/Version.rc.in" "${UE4SSLFB_RESOURCE}" @ONLY)
    find_program(MSVC_LIB_EXE lib.exe REQUIRED)
    set(UE4SSLFB_IMPORT_LIB "${CMAKE_CURRENT_BINARY_DIR}/UE4SS-97b7e501.lib")
    add_custom_command(OUTPUT "${UE4SSLFB_IMPORT_LIB}"
        COMMAND "${MSVC_LIB_EXE}" /nologo /machine:x64 "/def:${CMAKE_CURRENT_SOURCE_DIR}/abi/UE4SS-97b7e501.def" "/out:${UE4SSLFB_IMPORT_LIB}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/abi/UE4SS-97b7e501.def" VERBATIM)
    add_custom_target(UE4SSLFBImportLibrary DEPENDS "${UE4SSLFB_IMPORT_LIB}")
    add_library(UE4SSLuaFileBridgeImplementation SHARED
        src/FileBridgeMod.cpp src/Win32Files.cpp "${UE4SSLFB_GENERATED_LUA_API}" "${UE4SSLFB_RESOURCE}")
    add_dependencies(UE4SSLuaFileBridgeImplementation UE4SSLFBImportLibrary)
    target_compile_features(UE4SSLuaFileBridgeImplementation PRIVATE cxx_std_20)
    target_compile_definitions(UE4SSLuaFileBridgeImplementation PRIVATE
        UE4SSLEB_IMPLEMENTATION_MAGIC=${UE4SSLFB_MAGIC} UNICODE _UNICODE)
    target_include_directories(UE4SSLuaFileBridgeImplementation PRIVATE
        "${UE4SSLFB_GENERATED_INCLUDE_DIR}" include contract "${UE4SSLFB_REPO_ROOT}/shared/contract")
    target_link_libraries(UE4SSLuaFileBridgeImplementation PRIVATE
        "${UE4SSLFB_IMPORT_LIB}" kernel32 shell32 ole32 UE4SSXBDispatch)
    target_compile_options(UE4SSLuaFileBridgeImplementation PRIVATE /W4 /WX /GR- /permissive- /utf-8)
    set_property(TARGET UE4SSLuaFileBridgeImplementation PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    set_target_properties(UE4SSLuaFileBridgeImplementation PROPERTIES
        OUTPUT_NAME "UE4SSLuaFileBridge-${UE4SSLFB_PRODUCT_VERSION}"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist/0_ModCore_UE4SSLuaFileBridge/dlls/versions")
endif()

if(UE4SSLEB_BUILD_TESTS)
    foreach(test IN ITEMS CodecTests PathTests PolicyTests AtomicWriteTests TailTests)
        add_executable(FileBridge${test} "${CMAKE_CURRENT_SOURCE_DIR}/tests/${test}.cpp")
        target_compile_features(FileBridge${test} PRIVATE cxx_std_20)
        target_include_directories(FileBridge${test} PRIVATE include)
        add_test(NAME FileBridge${test} COMMAND FileBridge${test})
    endforeach()
endif()
