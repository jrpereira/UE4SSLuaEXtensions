cmake_minimum_required(VERSION 3.22)

get_filename_component(UE4SSLFB_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${UE4SSLFB_REPO_ROOT}/shared/bootstrap/cmake/BridgeBootstrap.cmake")
include("${UE4SSLFB_REPO_ROOT}/shared/cmake/LexProduct.cmake")
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

lex_selector_file(UE4SSLFB_SELECTOR VERSION ${UE4SSLFB_PRODUCT_VERSION})
add_bridge_bootstrap(
    TARGET UE4SSLFBBootstrap
    PRODUCT UE4SSLuaFileBridge
    DISPLAY_NAME "UE4SS Lua File Bridge"
    MAGIC ${UE4SSLFB_MAGIC}
    CLAIM_NAME UE4SSLFB
    VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/contract/Version.hpp"
    DIST_FOLDER 0_ModCore_UE4SSLuaFileBridge
    SELECTOR_FILE "${UE4SSLFB_SELECTOR}"
    ENABLED_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/enabled.txt"
    SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/lua/main_files.lua"
    SCRIPT_MODULES "${LEX_SHARED_DIR}/lua/lex_verify.lua"
    SETTINGS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/mod_settings.ini"
)

lex_embed_lua(UE4SSLFB_GENERATED_LUA_API
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/lua/files_api.lua"
    NAMESPACE UE4SSLuaFileBridge
    DELIMITER UE4SSLFB_LUA)
get_filename_component(UE4SSLFB_GENERATED_INCLUDE_DIR "${UE4SSLFB_GENERATED_LUA_API}" DIRECTORY)

if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR "The ABI-pinned UE4SS target must be built with MSVC.")
    endif()
    enable_language(RC)
    lex_implementation_resource(UE4SSLFB_RESOURCE
        PRODUCT UE4SSLuaFileBridge
        DISPLAY_NAME "UE4SS Lua File Bridge"
        VERSION ${UE4SSLFB_PRODUCT_VERSION})
    lex_ue4ss_import_library(UE4SSLFB_IMPORT_LIB UE4SSLFB_IMPORT_TARGET)
    add_library(UE4SSLuaFileBridgeImplementation SHARED
        src/FileBridgeMod.cpp src/Win32Files.cpp "${UE4SSLFB_GENERATED_LUA_API}" "${UE4SSLFB_RESOURCE}")
    add_dependencies(UE4SSLuaFileBridgeImplementation ${UE4SSLFB_IMPORT_TARGET})
    target_compile_features(UE4SSLuaFileBridgeImplementation PRIVATE cxx_std_20)
    target_compile_definitions(UE4SSLuaFileBridgeImplementation PRIVATE
        UE4SSLEB_IMPLEMENTATION_MAGIC=${UE4SSLFB_MAGIC} UNICODE _UNICODE)
    target_include_directories(UE4SSLuaFileBridgeImplementation PRIVATE
        "${UE4SSLFB_GENERATED_INCLUDE_DIR}" include contract "${LEX_SHARED_DIR}/contract" "${LEX_SHARED_DIR}/ue4ss-abi"
        "${LEX_SHARED_DIR}/native")
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
        target_include_directories(FileBridge${test} PRIVATE include "${LEX_SHARED_DIR}/tests")
        add_test(NAME FileBridge${test} COMMAND FileBridge${test})
    endforeach()
endif()
