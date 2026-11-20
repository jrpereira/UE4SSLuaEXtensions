cmake_minimum_required(VERSION 3.22)

get_filename_component(UE4SSLLB_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${UE4SSLLB_REPO_ROOT}/shared/bootstrap/cmake/BridgeBootstrap.cmake")
include("${UE4SSLLB_REPO_ROOT}/shared/cmake/LexProduct.cmake")
bridge_read_product_version(UE4SSLLB_PRODUCT_VERSION "${CMAKE_CURRENT_LIST_DIR}/contract/Version.hpp")
project(UE4SSLuaLogBridgeImplementation VERSION ${UE4SSLLB_PRODUCT_VERSION} LANGUAGES CXX)

option(UE4SSLEB_BUILD_TESTS "Build portable unit tests" ON)
if(UE4SSLEB_BUILD_TESTS AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
endif()

# Descriptor magic 'LLB1': the bootstrap and the implementation must agree, and
# LogBridgeMod.cpp checks it against NativeContract::implementation_magic.
set(UE4SSLLB_MAGIC 0x4C4C4231)

lex_selector_file(UE4SSLLB_SELECTOR VERSION ${UE4SSLLB_PRODUCT_VERSION})
add_bridge_bootstrap(
    TARGET UE4SSLLBBootstrap
    PRODUCT UE4SSLuaLogBridge
    DISPLAY_NAME "UE4SS Lua Log Bridge"
    MAGIC ${UE4SSLLB_MAGIC}
    CLAIM_NAME UE4SSLLB
    VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/contract/Version.hpp"
    DIST_FOLDER 0_ModCore_UE4SSLuaLogBridge
    SELECTOR_FILE "${UE4SSLLB_SELECTOR}"
    ENABLED_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/enabled.txt"
    SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/lua/main_logs.lua"
    SCRIPT_MODULES "${LEX_SHARED_DIR}/lua/lex_verify.lua"
    MANIFEST_FILE "${CMAKE_CURRENT_SOURCE_DIR}/mod.json"
)

lex_embed_lua(UE4SSLLB_GENERATED_LUA_API
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/lua/logs_api.lua"
    NAMESPACE UE4SSLuaLogBridge
    DELIMITER UE4SSLLB_LUA)
get_filename_component(UE4SSLLB_GENERATED_INCLUDE_DIR "${UE4SSLLB_GENERATED_LUA_API}" DIRECTORY)

if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR "The ABI-pinned UE4SS target must be built with MSVC.")
    endif()
    enable_language(RC)
    lex_implementation_resource(UE4SSLLB_RESOURCE
        PRODUCT UE4SSLuaLogBridge
        DISPLAY_NAME "UE4SS Lua Log Bridge"
        VERSION ${UE4SSLLB_PRODUCT_VERSION})
    lex_ue4ss_import_library(UE4SSLLB_IMPORT_LIB UE4SSLLB_IMPORT_TARGET)
    add_library(UE4SSLuaLogBridgeImplementation SHARED
        src/LogBridgeMod.cpp src/Win32Logs.cpp
        "${UE4SSLLB_GENERATED_LUA_API}" "${UE4SSLLB_RESOURCE}")
    add_dependencies(UE4SSLuaLogBridgeImplementation ${UE4SSLLB_IMPORT_TARGET})
    target_compile_features(UE4SSLuaLogBridgeImplementation PRIVATE cxx_std_20)
    target_compile_definitions(UE4SSLuaLogBridgeImplementation PRIVATE
        UE4SSLEB_IMPLEMENTATION_MAGIC=${UE4SSLLB_MAGIC} UNICODE _UNICODE)
    target_include_directories(UE4SSLuaLogBridgeImplementation PRIVATE
        "${UE4SSLLB_GENERATED_INCLUDE_DIR}" include contract "${LEX_SHARED_DIR}/contract" "${LEX_SHARED_DIR}/ue4ss-abi"
        "${LEX_SHARED_DIR}/native")
    target_link_libraries(UE4SSLuaLogBridgeImplementation PRIVATE
        "${UE4SSLLB_IMPORT_LIB}" kernel32)
    target_compile_options(UE4SSLuaLogBridgeImplementation PRIVATE /W4 /WX /GR- /permissive- /utf-8)
    set_property(TARGET UE4SSLuaLogBridgeImplementation PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    set_target_properties(UE4SSLuaLogBridgeImplementation PROPERTIES
        OUTPUT_NAME "UE4SSLuaLogBridge-${UE4SSLLB_PRODUCT_VERSION}"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist/0_ModCore_UE4SSLuaLogBridge/dlls/versions")
endif()

if(UE4SSLEB_BUILD_TESTS)
    find_package(Threads REQUIRED)
    foreach(test IN ITEMS LevelsTests JournalTests CoreTests)
        add_executable(LogBridge${test} "${CMAKE_CURRENT_SOURCE_DIR}/tests/${test}.cpp")
        target_compile_features(LogBridge${test} PRIVATE cxx_std_20)
        target_include_directories(LogBridge${test} PRIVATE include "${LEX_SHARED_DIR}/tests")
        target_link_libraries(LogBridge${test} PRIVATE Threads::Threads)
        add_test(NAME LogBridge${test} COMMAND LogBridge${test})
    endforeach()
endif()
