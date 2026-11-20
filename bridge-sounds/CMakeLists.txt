cmake_minimum_required(VERSION 3.22)

get_filename_component(UE4SSLSB_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${UE4SSLSB_REPO_ROOT}/shared/bootstrap/cmake/BridgeBootstrap.cmake")
include("${UE4SSLSB_REPO_ROOT}/shared/cmake/LexProduct.cmake")
bridge_read_product_version(UE4SSLSB_PRODUCT_VERSION "${CMAKE_CURRENT_LIST_DIR}/contract/Version.hpp")
project(UE4SSLuaSoundBridgeImplementation VERSION ${UE4SSLSB_PRODUCT_VERSION} LANGUAGES CXX)

option(UE4SSLEB_BUILD_TESTS "Build portable unit tests" ON)
if(UE4SSLEB_BUILD_TESTS AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
endif()

# Descriptor magic 'LSB1': the bootstrap and the implementation must agree, and
# SoundBridgeMod.cpp checks it against NativeContract::implementation_magic.
set(UE4SSLSB_MAGIC 0x4C534231)

lex_selector_file(UE4SSLSB_SELECTOR VERSION ${UE4SSLSB_PRODUCT_VERSION})
add_bridge_bootstrap(
    TARGET UE4SSLSBBootstrap
    PRODUCT UE4SSLuaSoundBridge
    DISPLAY_NAME "UE4SS Lua Sound Bridge"
    MAGIC ${UE4SSLSB_MAGIC}
    CLAIM_NAME UE4SSLSB
    VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/contract/Version.hpp"
    DIST_FOLDER 0_ModCore_UE4SSLuaSoundBridge
    SELECTOR_FILE "${UE4SSLSB_SELECTOR}"
    ENABLED_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/enabled.txt"
    SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/lua/main_sounds.lua"
    SCRIPT_MODULES "${LEX_SHARED_DIR}/lua/lex_verify.lua"
    SETTINGS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/mod_settings.ini"
)

lex_embed_lua(UE4SSLSB_GENERATED_LUA_API
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/lua/sounds_api.lua"
    NAMESPACE UE4SSLuaSoundBridge
    DELIMITER UE4SSLSB_LUA)
get_filename_component(UE4SSLSB_GENERATED_INCLUDE_DIR "${UE4SSLSB_GENERATED_LUA_API}" DIRECTORY)

if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR "The ABI-pinned UE4SS target must be built with MSVC.")
    endif()
    enable_language(RC)
    lex_implementation_resource(UE4SSLSB_RESOURCE
        PRODUCT UE4SSLuaSoundBridge
        DISPLAY_NAME "UE4SS Lua Sound Bridge"
        VERSION ${UE4SSLSB_PRODUCT_VERSION})
    lex_ue4ss_import_library(UE4SSLSB_IMPORT_LIB UE4SSLSB_IMPORT_TARGET)
    add_library(UE4SSLuaSoundBridgeImplementation SHARED
        src/SoundBridgeMod.cpp src/Win32Sounds.cpp src/XAudio2Engine.cpp
        "${UE4SSLSB_GENERATED_LUA_API}" "${UE4SSLSB_RESOURCE}")
    add_dependencies(UE4SSLuaSoundBridgeImplementation ${UE4SSLSB_IMPORT_TARGET})
    target_compile_features(UE4SSLuaSoundBridgeImplementation PRIVATE cxx_std_20)
    target_compile_definitions(UE4SSLuaSoundBridgeImplementation PRIVATE
        UE4SSLEB_IMPLEMENTATION_MAGIC=${UE4SSLSB_MAGIC} UNICODE _UNICODE)
    target_include_directories(UE4SSLuaSoundBridgeImplementation PRIVATE
        "${UE4SSLSB_GENERATED_INCLUDE_DIR}" include contract "${LEX_SHARED_DIR}/contract" "${LEX_SHARED_DIR}/ue4ss-abi"
        "${LEX_SHARED_DIR}/native")
    # xaudio2.lib binds xaudio2_9.dll, part of Windows 10 and later.
    target_link_libraries(UE4SSLuaSoundBridgeImplementation PRIVATE
        "${UE4SSLSB_IMPORT_LIB}" kernel32 ole32 xaudio2)
    target_compile_options(UE4SSLuaSoundBridgeImplementation PRIVATE /W4 /WX /GR- /permissive- /utf-8)
    set_property(TARGET UE4SSLuaSoundBridgeImplementation PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    set_target_properties(UE4SSLuaSoundBridgeImplementation PROPERTIES
        OUTPUT_NAME "UE4SSLuaSoundBridge-${UE4SSLSB_PRODUCT_VERSION}"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist/0_ModCore_UE4SSLuaSoundBridge/dlls/versions")
endif()

if(UE4SSLEB_BUILD_TESTS)
    foreach(test IN ITEMS WavTests PathTests MixTests VoicePoolTests)
        add_executable(SoundBridge${test} "${CMAKE_CURRENT_SOURCE_DIR}/tests/${test}.cpp")
        target_compile_features(SoundBridge${test} PRIVATE cxx_std_20)
        target_include_directories(SoundBridge${test} PRIVATE include "${LEX_SHARED_DIR}/tests")
        add_test(NAME SoundBridge${test} COMMAND SoundBridge${test})
    endforeach()
endif()
