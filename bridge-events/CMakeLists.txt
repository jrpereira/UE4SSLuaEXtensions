cmake_minimum_required(VERSION 3.22)

get_filename_component(UE4SSLEB_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT DEFINED UE4SSLEB_PRODUCT_VERSION)
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/contract/Version.hpp" UE4SSLEB_VERSION_LINE REGEX "^#define UE4SSLEB_VERSION ")
    if(NOT UE4SSLEB_VERSION_LINE MATCHES "\"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
        message(FATAL_ERROR "Invalid product version in bridge-events/contract/Version.hpp")
    endif()
    set(UE4SSLEB_PRODUCT_VERSION "${CMAKE_MATCH_1}")
endif()
project(UE4SSLuaEventBridgeImplementation VERSION ${UE4SSLEB_PRODUCT_VERSION} LANGUAGES CXX)

option(UE4SSLEB_BUILD_TESTS "Build portable unit tests" ON)
option(UE4SSLEB_COMPONENT_LAYOUT_128 "Experimental UE5.5 component layout for isolated sample tests" OFF)
if(UE4SSLEB_BUILD_TESTS AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
endif()
if(NOT TARGET UE4SSXBDispatch)
    add_subdirectory("${UE4SSLEB_REPO_ROOT}/shared/dispatch" shared-dispatch)
endif()

include("${UE4SSLEB_REPO_ROOT}/bootstrap/cmake/BridgeBootstrap.cmake")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/main.json.in" "${CMAKE_CURRENT_BINARY_DIR}/main.json" @ONLY)
add_bridge_bootstrap(
    TARGET UE4SSLEBBootstrap
    PRODUCT UE4SSLuaEventBridge
    DISPLAY_NAME "UE4SS Lua Event Bridge"
    MAGIC 0x4C454231
    CLAIM_NAME UE4SSLEB
    VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/contract/Version.hpp"
    DIST_FOLDER 0_ModCore_UE4SSLuaEventBridge
    SELECTOR_FILE "${CMAKE_CURRENT_BINARY_DIR}/main.json"
    ENABLED_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/enabled.txt"
    SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/lua/main_events.lua"
    SETTINGS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/mod_settings.ini"
)

set(UE4SSLEB_LUA_API_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/lua/bridge_api.lua")
set(UE4SSLEB_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(UE4SSLEB_GENERATED_LUA_API "${UE4SSLEB_GENERATED_INCLUDE_DIR}/EmbeddedLuaAPI.hpp")
file(MAKE_DIRECTORY "${UE4SSLEB_GENERATED_INCLUDE_DIR}")
file(READ "${UE4SSLEB_LUA_API_SOURCE}" UE4SSLEB_LUA_API)
# MSVC limits a single string literal to about 16 KB, so the Lua API is
# embedded as as many fixed-size raw-string chunks as its length needs.
set(UE4SSLEB_LUA_API_CHUNK_SIZE 7000)
string(LENGTH "${UE4SSLEB_LUA_API}" UE4SSLEB_LUA_API_LENGTH)
string(FIND "${UE4SSLEB_LUA_API}" ")UE4SSLEB_LUA" UE4SSLEB_LUA_API_DELIMITER)
if(NOT UE4SSLEB_LUA_API_DELIMITER EQUAL -1)
    message(FATAL_ERROR "bridge_api.lua contains the raw-string delimiter )UE4SSLEB_LUA")
endif()
set(UE4SSLEB_LUA_API_CHUNKS "")
set(UE4SSLEB_LUA_API_CHUNK_COUNT 0)
set(UE4SSLEB_LUA_API_OFFSET 0)
while(UE4SSLEB_LUA_API_OFFSET LESS UE4SSLEB_LUA_API_LENGTH)
    string(SUBSTRING "${UE4SSLEB_LUA_API}" ${UE4SSLEB_LUA_API_OFFSET} ${UE4SSLEB_LUA_API_CHUNK_SIZE}
        UE4SSLEB_LUA_API_CHUNK)
    string(APPEND UE4SSLEB_LUA_API_CHUNKS "    R\"UE4SSLEB_LUA(${UE4SSLEB_LUA_API_CHUNK})UE4SSLEB_LUA\",\n")
    math(EXPR UE4SSLEB_LUA_API_OFFSET "${UE4SSLEB_LUA_API_OFFSET} + ${UE4SSLEB_LUA_API_CHUNK_SIZE}")
    math(EXPR UE4SSLEB_LUA_API_CHUNK_COUNT "${UE4SSLEB_LUA_API_CHUNK_COUNT} + 1")
endwhile()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbeddedLuaAPI.hpp.in" "${UE4SSLEB_GENERATED_LUA_API}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${UE4SSLEB_LUA_API_SOURCE}")

if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR "The ABI-pinned UE4SS target must be built with MSVC.")
    endif()
    enable_language(RC)
    set(UE4SSLEB_BRIDGE_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/Version.rc")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/Version.rc.in" "${UE4SSLEB_BRIDGE_RESOURCE}" @ONLY)
    find_program(MSVC_LIB_EXE lib.exe REQUIRED)
    set(UE4SS_IMPORT_LIB "${CMAKE_CURRENT_BINARY_DIR}/UE4SS-97b7e501.lib")
    add_custom_command(OUTPUT "${UE4SS_IMPORT_LIB}"
        COMMAND "${MSVC_LIB_EXE}" /nologo /machine:x64 "/def:${CMAKE_CURRENT_SOURCE_DIR}/abi/UE4SS-97b7e501.def" "/out:${UE4SS_IMPORT_LIB}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/abi/UE4SS-97b7e501.def" VERBATIM)
    add_custom_target(UE4SSImportLibrary DEPENDS "${UE4SS_IMPORT_LIB}")
    add_library(UE4SSLuaEventBridgeImplementation SHARED
        src/BridgeMod.cpp src/EnhancedInputBackend.cpp "${UE4SSLEB_GENERATED_LUA_API}" "${UE4SSLEB_BRIDGE_RESOURCE}")
    add_dependencies(UE4SSLuaEventBridgeImplementation UE4SSImportLibrary)
    target_compile_features(UE4SSLuaEventBridgeImplementation PRIVATE cxx_std_20)
    if(UE4SSLEB_COMPONENT_LAYOUT_128)
        target_compile_definitions(UE4SSLuaEventBridgeImplementation PRIVATE UE4SSLEB_COMPONENT_LAYOUT_128=1)
    endif()
    target_include_directories(UE4SSLuaEventBridgeImplementation PRIVATE
        "${UE4SSLEB_GENERATED_INCLUDE_DIR}" include contract "${UE4SSLEB_REPO_ROOT}/contract")
    target_link_libraries(UE4SSLuaEventBridgeImplementation PRIVATE "${UE4SS_IMPORT_LIB}" kernel32 UE4SSXBDispatch)
    target_compile_options(UE4SSLuaEventBridgeImplementation PRIVATE /W4 /WX /GR- /permissive-)
    set_property(TARGET UE4SSLuaEventBridgeImplementation PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    set_target_properties(UE4SSLuaEventBridgeImplementation PROPERTIES
        OUTPUT_NAME "UE4SSLuaEventBridge-${UE4SSLEB_PRODUCT_VERSION}"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist/0_ModCore_UE4SSLuaEventBridge/dlls/versions")
endif()

if(UE4SSLEB_BUILD_TESTS)
    set(UE4SSLEB_TEST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    foreach(test IN ITEMS WeakObjectPtr BindingSnapshot)
        add_executable(${test}Tests "${UE4SSLEB_TEST_ROOT}/${test}Tests.cpp")
        target_compile_features(${test}Tests PRIVATE cxx_std_20)
        target_include_directories(${test}Tests PRIVATE include "${UE4SSLEB_REPO_ROOT}/contract")
        add_test(NAME ${test}Tests COMMAND ${test}Tests)
    endforeach()
    find_package(Threads REQUIRED)
    foreach(test IN ITEMS SessionAliasIndex LifecycleRegistry LifetimeProbe)
        add_executable(${test}Tests "${UE4SSLEB_TEST_ROOT}/${test}Tests.cpp")
        target_compile_features(${test}Tests PRIVATE cxx_std_20)
        target_include_directories(${test}Tests PRIVATE include "${UE4SSLEB_REPO_ROOT}/contract")
        target_link_libraries(${test}Tests PRIVATE Threads::Threads)
        add_test(NAME ${test}Tests COMMAND ${test}Tests)
    endforeach()
    if(WIN32)
        add_executable(NativeBackendLifecycleTests "${UE4SSLEB_TEST_ROOT}/NativeBackendLifecycleTests.cpp")
        target_compile_features(NativeBackendLifecycleTests PRIVATE cxx_std_20)
        target_include_directories(NativeBackendLifecycleTests PRIVATE include "${UE4SSLEB_REPO_ROOT}/contract")
        target_link_libraries(NativeBackendLifecycleTests PRIVATE UE4SSXBDispatch)
        target_compile_options(NativeBackendLifecycleTests PRIVATE /W4 /WX)
        add_test(NAME NativeBackendLifecycleTests COMMAND NativeBackendLifecycleTests)
        set_tests_properties(NativeBackendLifecycleTests PROPERTIES TIMEOUT 20)
    endif()
endif()
