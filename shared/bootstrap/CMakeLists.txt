cmake_minimum_required(VERSION 3.22)

# The generic bootstrap. Each product creates its main.dll with
# add_bridge_bootstrap (cmake/BridgeBootstrap.cmake) in its own CMakeLists.txt.
project(UE4SSXBBootstrap LANGUAGES CXX)

option(UE4SSLEB_BUILD_TESTS "Build portable unit tests" ON)
if(UE4SSLEB_BUILD_TESTS AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
endif()

if(UE4SSLEB_BUILD_TESTS)
    add_executable(BootstrapSelectionTests tests/BootstrapSelectionTests.cpp)
    target_compile_features(BootstrapSelectionTests PRIVATE cxx_std_20)
    target_include_directories(BootstrapSelectionTests PRIVATE include)
    add_test(NAME BootstrapSelectionTests COMMAND BootstrapSelectionTests)
endif()
