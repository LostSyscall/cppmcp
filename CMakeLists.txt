cmake_minimum_required(VERSION 3.15)
project(cppmcp VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(llhttp CONFIG REQUIRED)

include(GNUInstallDirs)

# 库类型由编译选项决定：默认静态（无需改动源码）。需在 add_subdirectory(src) 之前生效，
# 因为 add_library 不指定类型时跟随标准变量 BUILD_SHARED_LIBS。
option(CPPMCP_BUILD_SHARED "Build cppmcp as shared libraries (.dll/.so) instead of static (.lib/.a)" OFF)
if(CPPMCP_BUILD_SHARED)
    set(BUILD_SHARED_LIBS ON)
endif()

# Windows 下源码无 dllexport 宏，SHARED 库靠此自动导出全部符号（静态库不受影响）。
if(WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

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

# ===== 安装 / 导出 / config 包 =====
include(CMakePackageConfigHelpers)

# 三个库各自独立安装，并汇入同一个 EXPORT（命名空间 cppmcp::）。
foreach(lib IN ITEMS common server client)
    install(TARGETS cppmcp_${lib}
            EXPORT cppmcpTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endforeach()

# 头文件整体安装（不拆分目录）。
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/cppmcp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT cppmcpTargets
        NAMESPACE cppmcp::
        FILE cppmcpTargets.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppmcp)

# 生成 config 包，供 find_package(cppmcp) / find_package(cppmcp COMPONENTS server client) 使用。
configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/cppmcp-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cppmcp-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppmcp)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cppmcp-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cppmcp-config.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/cppmcp-config-version.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppmcp)
