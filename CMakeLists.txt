cmake_minimum_required(VERSION 3.20)
project(BCNP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BCNP_SCHEMA_FILE "${CMAKE_CURRENT_SOURCE_DIR}/schema/messages.json" CACHE FILEPATH "Path to BCNP message schema JSON file")
set(BCNP_CODEGEN_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/schema/bcnp_codegen.py")
set(BCNP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated" CACHE PATH "Output directory for generated BCNP headers")
set(BCNP_GENERATED_HEADER "${BCNP_GENERATED_DIR}/bcnp/message_types.h")

# Test schema generates test message types
set(BCNP_TEST_SCHEMA_FILE "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_schema.json")
set(BCNP_TEST_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/tests/generated")
set(BCNP_TEST_GENERATED_HEADER "${BCNP_TEST_GENERATED_DIR}/bcnp/message_types.h")

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Generate user message types (empty by default)
add_custom_command(
    OUTPUT "${BCNP_GENERATED_HEADER}"
    COMMAND ${Python3_EXECUTABLE} "${BCNP_CODEGEN_SCRIPT}" 
            "${BCNP_SCHEMA_FILE}" 
            --cpp "${BCNP_GENERATED_DIR}"
    DEPENDS "${BCNP_SCHEMA_FILE}" "${BCNP_CODEGEN_SCRIPT}"
    COMMENT "Generating BCNP message types from schema..."
    VERBATIM
)

# Generate test message types
add_custom_command(
    OUTPUT "${BCNP_TEST_GENERATED_HEADER}"
    COMMAND ${Python3_EXECUTABLE} "${BCNP_CODEGEN_SCRIPT}" 
            "${BCNP_TEST_SCHEMA_FILE}" 
            --cpp "${BCNP_TEST_GENERATED_DIR}"
    DEPENDS "${BCNP_TEST_SCHEMA_FILE}" "${BCNP_CODEGEN_SCRIPT}"
    COMMENT "Generating BCNP test message types..."
    VERBATIM
)

add_custom_target(bcnp_codegen DEPENDS "${BCNP_GENERATED_HEADER}")
add_custom_target(bcnp_test_codegen DEPENDS "${BCNP_TEST_GENERATED_HEADER}")

set(BCNP_CORE_SOURCES
    src/bcnp/packet.cpp
    src/bcnp/stream_parser.cpp
    src/bcnp/dispatcher.cpp
    src/bcnp/transport/controller_driver.cpp
    src/bcnp/spi_adapter.cpp) # spi deprecated, remove later

if(UNIX)
    list(APPEND BCNP_CORE_SOURCES src/bcnp/transport/udp_posix.cpp src/bcnp/transport/tcp_posix.cpp)
else()
    message(STATUS "Skipping POSIX transport adapters on non-UNIX platform")
endif()

add_library(bcnp_core ${BCNP_CORE_SOURCES})
add_dependencies(bcnp_core bcnp_codegen)

# CrabLib integration - memory safety primitives
add_subdirectory(libraries/crablib)

target_include_directories(bcnp_core PUBLIC src "${BCNP_GENERATED_DIR}")
target_link_libraries(bcnp_core PUBLIC crab::crab)

target_compile_features(bcnp_core PUBLIC cxx_std_17)

# Test library uses test-generated types
add_library(bcnp_test_types INTERFACE)
add_dependencies(bcnp_test_types bcnp_test_codegen)
target_include_directories(bcnp_test_types INTERFACE "${BCNP_TEST_GENERATED_DIR}")

add_executable(bcnp_tests_1 tests/bcnp_tests_1.cpp)
target_link_libraries(bcnp_tests_1 PRIVATE bcnp_core bcnp_test_types)
# Override include dirs so test types take precedence
target_include_directories(bcnp_tests_1 PRIVATE "${BCNP_TEST_GENERATED_DIR}" src)

add_executable(bcnp_tests_2 tests/bcnp_tests_2.cpp)
target_link_libraries(bcnp_tests_2 PRIVATE bcnp_core bcnp_test_types)
target_include_directories(bcnp_tests_2 PRIVATE "${BCNP_TEST_GENERATED_DIR}" src tests)

if(UNIX)
    add_executable(core_demo examples/core_demo.cpp)
    target_link_libraries(core_demo PRIVATE bcnp_core)
endif()

enable_testing()
add_test(NAME bcnp_tests_1 COMMAND bcnp_tests_1)
add_test(NAME bcnp_tests_2 COMMAND bcnp_tests_2)