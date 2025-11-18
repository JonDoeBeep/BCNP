cmake_minimum_required(VERSION 3.20)
project(BCNP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BCNP_CORE_SOURCES
    src/bcnp/packet.cpp
    src/bcnp/command_queue.cpp
    src/bcnp/stream_parser.cpp
    src/bcnp/controller.cpp
    src/bcnp/transport/controller_driver.cpp)

if(UNIX)
    list(APPEND BCNP_CORE_SOURCES src/bcnp/transport/udp_posix.cpp)
else()
    message(STATUS "Skipping POSIX UDP adapter on non-UNIX platform")
endif()

add_library(bcnp_core ${BCNP_CORE_SOURCES})

target_include_directories(bcnp_core PUBLIC src)

target_compile_features(bcnp_core PUBLIC cxx_std_17)

add_executable(bcnp_tests tests/bcnp_tests.cpp)

target_link_libraries(bcnp_tests PRIVATE bcnp_core)

enable_testing()
add_test(NAME bcnp_tests COMMAND bcnp_tests)
