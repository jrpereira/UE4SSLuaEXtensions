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

include("${UE4SSLEB_REPO_ROOT}/shared/bootstrap/cmake/BridgeBootstrap.cmake")
include("${UE4SSLEB_REPO_ROOT}/shared/cmake/LexProduct.cmake")
lex_selector_file(UE4SSLEB_SELECTOR VERSION ${UE4SSLEB_PRODUCT_VERSION})
add_bridge_bootstrap(
    TARGET UE4SSLEBBootstrap
    PRODUCT UE4SSLuaEventBridge
    DISPLAY_NAME "UE4SS Lua Event Bridge"
    MAGIC 0x4C454231
    CLAIM_NAME UE4SSLEB
    VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/contract/Version.hpp"
    DIST_FOLDER 0_ModCore_UE4SSLuaEventBridge
    SELECTOR_FILE "${UE4SSLEB_SELECTOR}"
    ENABLED_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/enabled.txt"
    SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/lua/main_events.lua"
    SCRIPT_MODULES "${LEX_SHARED_DIR}/lua/lex_verify.lua"
    SETTINGS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/mod_settings.ini"
)

lex_embed_lua(UE4SSLEB_GENERATED_LUA_API
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/lua/bridge_api.lua"
    NAMESPACE UE4SSLuaEventBridge
    DELIMITER UE4SSLEB_LUA)
get_filename_component(UE4SSLEB_GENERATED_INCLUDE_DIR "${UE4SSLEB_GENERATED_LUA_API}" DIRECTORY)

if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR "The ABI-pinned UE4SS target must be built with MSVC.")
    endif()
    enable_language(RC)
    lex_implementation_resource(UE4SSLEB_BRIDGE_RESOURCE
        PRODUCT UE4SSLuaEventBridge
        DISPLAY_NAME "UE4SS Lua Event Bridge"
        VERSION ${UE4SSLEB_PRODUCT_VERSION})
    lex_ue4ss_import_library(UE4SS_IMPORT_LIB UE4SS_IMPORT_TARGET)
    add_library(UE4SSLuaEventBridgeImplementation SHARED
        src/BridgeMod.cpp src/EnhancedInputBackend.cpp src/HookCore.cpp src/ParamSchema.cpp
        "${UE4SSLEB_GENERATED_LUA_API}" "${UE4SSLEB_BRIDGE_RESOURCE}")
    add_dependencies(UE4SSLuaEventBridgeImplementation ${UE4SS_IMPORT_TARGET})
    target_compile_features(UE4SSLuaEventBridgeImplementation PRIVATE cxx_std_20)
    if(UE4SSLEB_COMPONENT_LAYOUT_128)
        target_compile_definitions(UE4SSLuaEventBridgeImplementation PRIVATE UE4SSLEB_COMPONENT_LAYOUT_128=1)
    endif()
    target_include_directories(UE4SSLuaEventBridgeImplementation PRIVATE
        "${UE4SSLEB_GENERATED_INCLUDE_DIR}" include contract "${LEX_SHARED_DIR}/contract" "${LEX_SHARED_DIR}/ue4ss-abi"
        "${LEX_SHARED_DIR}/native")
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
        target_include_directories(${test}Tests PRIVATE include "${LEX_SHARED_DIR}/contract" "${LEX_SHARED_DIR}/ue4ss-abi"
            "${LEX_SHARED_DIR}/tests")
        add_test(NAME ${test}Tests COMMAND ${test}Tests)
    endforeach()
    find_package(Threads REQUIRED)
    foreach(test IN ITEMS SessionAliasIndex LifecycleRegistry LifetimeProbe)
        add_executable(${test}Tests "${UE4SSLEB_TEST_ROOT}/${test}Tests.cpp")
        target_compile_features(${test}Tests PRIVATE cxx_std_20)
        target_include_directories(${test}Tests PRIVATE include "${LEX_SHARED_DIR}/contract" "${LEX_SHARED_DIR}/ue4ss-abi"
            "${LEX_SHARED_DIR}/tests")
        target_link_libraries(${test}Tests PRIVATE Threads::Threads)
        add_test(NAME ${test}Tests COMMAND ${test}Tests)
    endforeach()
    # Function-hook suites (COORDINATOR/LEX/LEX_hooks.md §11). Listed ahead of the
    # tracks that write them, so no track edits this file; a missing suite is skipped.
    foreach(test IN ITEMS OpcodeSlots ScriptPatch ReentryGuard HookTable ParamSchema ParamCopier HookFilters)
        if(EXISTS "${UE4SSLEB_TEST_ROOT}/${test}Tests.cpp")
            add_executable(${test}Tests "${UE4SSLEB_TEST_ROOT}/${test}Tests.cpp")
            target_compile_features(${test}Tests PRIVATE cxx_std_20)
            target_include_directories(${test}Tests PRIVATE include "${LEX_SHARED_DIR}/contract" "${LEX_SHARED_DIR}/ue4ss-abi"
                "${LEX_SHARED_DIR}/tests" "${UE4SSLEB_TEST_ROOT}/support")
            target_link_libraries(${test}Tests PRIVATE Threads::Threads)
            add_test(NAME ${test}Tests COMMAND ${test}Tests)
        endif()
    endforeach()
    if(WIN32 AND EXISTS "${UE4SSLEB_TEST_ROOT}/NativeHooksLifecycleWinTests.cpp")
        add_executable(NativeHooksLifecycleTests "${UE4SSLEB_TEST_ROOT}/NativeHooksLifecycleWinTests.cpp")
        target_compile_features(NativeHooksLifecycleTests PRIVATE cxx_std_20)
        target_include_directories(NativeHooksLifecycleTests PRIVATE include "${LEX_SHARED_DIR}/contract" "${LEX_SHARED_DIR}/ue4ss-abi"
            "${LEX_SHARED_DIR}/tests" "${UE4SSLEB_TEST_ROOT}/support")
        target_link_libraries(NativeHooksLifecycleTests PRIVATE UE4SSXBDispatch)
        target_compile_options(NativeHooksLifecycleTests PRIVATE /W4 /WX)
        add_test(NAME NativeHooksLifecycleTests COMMAND NativeHooksLifecycleTests)
        set_tests_properties(NativeHooksLifecycleTests PROPERTIES TIMEOUT 20)
    endif()
    if(WIN32)
        add_executable(NativeBackendLifecycleTests "${UE4SSLEB_TEST_ROOT}/NativeBackendLifecycleWinTests.cpp")
        target_compile_features(NativeBackendLifecycleTests PRIVATE cxx_std_20)
        target_include_directories(NativeBackendLifecycleTests PRIVATE include "${LEX_SHARED_DIR}/contract" "${LEX_SHARED_DIR}/ue4ss-abi"
            "${LEX_SHARED_DIR}/tests")
        target_link_libraries(NativeBackendLifecycleTests PRIVATE UE4SSXBDispatch)
        target_compile_options(NativeBackendLifecycleTests PRIVATE /W4 /WX)
        add_test(NAME NativeBackendLifecycleTests COMMAND NativeBackendLifecycleTests)
        set_tests_properties(NativeBackendLifecycleTests PROPERTIES TIMEOUT 20)
    endif()
endif()
