# Queue and dispatch code shared by the bridge products. Header-only.
# Link UE4SSXBDispatch and include <dispatch/Name.hpp>; names live in UE4SSXB.
add_library(UE4SSXBDispatch INTERFACE)
target_include_directories(UE4SSXBDispatch INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(UE4SSXBDispatch INTERFACE cxx_std_20)

if(UE4SSLEB_BUILD_TESTS)
    find_package(Threads REQUIRED)
    foreach(test IN ITEMS DispatchBudget QueueDispatchSchedule QueueBuffers)
        add_executable(${test}Tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/${test}Tests.cpp")
        target_link_libraries(${test}Tests PRIVATE UE4SSXBDispatch)
        target_include_directories(${test}Tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tests")
        add_test(NAME ${test}Tests COMMAND ${test}Tests)
    endforeach()
    target_link_libraries(QueueBuffersTests PRIVATE Threads::Threads)
    set_tests_properties(QueueBuffersTests PROPERTIES TIMEOUT 20)
endif()
