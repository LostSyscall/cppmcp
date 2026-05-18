cmake_minimum_required(VERSION 3.15)
project(cppmcp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(llhttp CONFIG REQUIRED)

add_subdirectory(src)

option(CPPMCP_BUILD_EXAMPLES "Build example servers" ON)
if(CPPMCP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

option(CPPMCP_BUILD_TESTS "Build unit tests" OFF)
if(CPPMCP_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests)
endif()

option(CPPMCP_BUILD_INTEGRATION_TESTS "Build integration tests (requires real I/O)" OFF)
if(CPPMCP_BUILD_INTEGRATION_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests/integration)
endif()