cmake_minimum_required(VERSION 3.10)
project(Nexus VERSION 3.0.0 LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Platform detection
if(CMAKE_SYSTEM_NAME STREQUAL "QNX")
    set(IS_QNX TRUE)
    add_definitions(-D__QNXNTO__)
    message(STATUS "Building for QNX platform")
else()
    set(IS_QNX FALSE)
    message(STATUS "Building for Linux platform")
endif()

# Options
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(BUILD_TESTS "Build test programs" ON)
option(BUILD_EXAMPLES "Build example programs" OFF)
option(ENABLE_COVERAGE "Enable code coverage generation" OFF)

if(ENABLE_COVERAGE)
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/nexus
)

# Backward compatibility: also include old paths
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Source files organized by module
set(NEXUS_CORE_SOURCES
    src/core/Config.cpp
    src/core/NodeImpl.cpp
)

set(NEXUS_TRANSPORT_SOURCES
    src/transport/SharedMemoryTransportV3.cpp
    src/transport/LargeDataChannel.cpp
    src/transport/UdpTransport.cpp
)

set(NEXUS_REGISTRY_SOURCES
    src/registry/SharedMemoryRegistry.cpp
    src/registry/GlobalRegistry.cpp
)

set(NEXUS_UTILS_SOURCES
    src/utils/Logger.cpp
)

# Combine all sources
set(NEXUS_SOURCES
    ${NEXUS_CORE_SOURCES}
    ${NEXUS_TRANSPORT_SOURCES}
    ${NEXUS_REGISTRY_SOURCES}
    ${NEXUS_UTILS_SOURCES}
)

# Create library
if(BUILD_SHARED_LIBS)
    add_library(nexus SHARED ${NEXUS_SOURCES})
    set_target_properties(nexus PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION 3
        OUTPUT_NAME "rpc"  # Keep librpc.so name for compatibility
    )
else()
    add_library(nexus STATIC ${NEXUS_SOURCES})
    set_target_properties(nexus PROPERTIES
        OUTPUT_NAME "rpc"
    )
endif()

# Link libraries
target_link_libraries(nexus
    pthread
    rt
)

# Compiler flags
target_compile_options(nexus PRIVATE
    -Wall
    -Wextra
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3>
)

# Installation
install(TARGETS nexus
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/nexus
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# Backward compatibility: install old headers
install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
    PATTERN "nexus" EXCLUDE
)

# Build tests
if(BUILD_TESTS)
    enable_testing()
    
    # Integration tests
    set(INTEGRATION_TESTS
        test_inprocess
        test_duplex_v2
        test_heartbeat_timeout
        test_service_discovery
        test_cross_process_discovery
        test_cross_process_node_events
    )
    
    foreach(test ${INTEGRATION_TESTS})
        add_executable(${test} tests/integration/${test}.cpp)
        target_link_libraries(${test} nexus pthread rt)
        add_test(NAME ${test} COMMAND ${test})
    endforeach()
    
    # Large data channel tests (require two processes)
    add_executable(test_large_sender tests/integration/test_large_sender.cpp)
    target_link_libraries(test_large_sender nexus pthread rt)
    
    add_executable(test_large_receiver tests/integration/test_large_receiver.cpp)
    target_link_libraries(test_large_receiver nexus pthread rt)
    
    # Special test for service cleanup verification
    add_executable(test_service_cleanup test_service_cleanup.cpp)
    target_link_libraries(test_service_cleanup nexus pthread rt)
    
    # Long-term stability test
    add_executable(test_long_term_stability tests/stability/test_long_term_stability.cpp)
    target_link_libraries(test_long_term_stability nexus pthread rt)
    
    # Polling overhead test
    add_executable(test_polling_overhead tests/stability/test_polling_overhead.cpp)
    target_link_libraries(test_polling_overhead nexus pthread rt)
    
    # Unit Tests
    set(UNIT_TEST_SOURCES
        tests/unit/main.cpp
        tests/unit/test_lock_free_queue.cpp
        tests/unit/test_config.cpp
        tests/unit/test_utils.cpp
        tests/unit/test_registry.cpp
        tests/unit/test_transport.cpp
        tests/unit/test_node.cpp
        tests/unit/test_large_data_channel.cpp
        tests/unit/test_transport_errors.cpp
        tests/unit/test_transport_extended.cpp
        tests/unit/test_node_extended.cpp
        tests/unit/test_node_udp.cpp
        tests/unit/test_node_edge_cases.cpp
        tests/unit/test_node_self_message.cpp
        tests/unit/test_coverage_gap.cpp
        tests/unit/test_transport_coverage.cpp
        tests/unit/test_transport_stress.cpp
        tests/unit/test_node_coverage.cpp
        tests/unit/test_registry_coverage.cpp
        tests/unit/test_node_impl_coverage.cpp
        tests/unit/test_transport_v3_coverage.cpp
        tests/unit/test_node_udp_coverage.cpp
        tests/unit/test_large_data_cleanup.cpp
        tests/unit/test_large_data_policies.cpp
        tests/unit/test_registry_advanced_coverage.cpp
        tests/unit/test_registry_coverage_extra.cpp
    )

    add_executable(unit_tests ${UNIT_TEST_SOURCES})
    target_include_directories(unit_tests PRIVATE tests/unit)
    target_link_libraries(unit_tests nexus pthread rt)
    add_test(NAME unit_tests COMMAND unit_tests)

    message(STATUS "Test programs will be built")
endif()

# Build examples
if(BUILD_EXAMPLES)
    message(STATUS "Example programs will be built")
    # TODO: Add examples
endif()

# Summary
message(STATUS "")
message(STATUS "═══════════════════════════════════════")
message(STATUS "  Nexus IPC Framework v${PROJECT_VERSION}")
message(STATUS "═══════════════════════════════════════")
message(STATUS "Build type:      ${CMAKE_BUILD_TYPE}")
message(STATUS "Shared libs:     ${BUILD_SHARED_LIBS}")
message(STATUS "Build tests:     ${BUILD_TESTS}")
message(STATUS "Build examples:  ${BUILD_EXAMPLES}")
message(STATUS "Platform:        ${CMAKE_SYSTEM_NAME}")
message(STATUS "Compiler:        ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "C++ standard:    C++${CMAKE_CXX_STANDARD}")
message(STATUS "Install prefix:  ${CMAKE_INSTALL_PREFIX}")
message(STATUS "═══════════════════════════════════════")
message(STATUS "")
