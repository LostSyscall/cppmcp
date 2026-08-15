# 公共代码（server 与 client 共用）。抽出独立库，避免把同一批符号
# 同时编进 server/client 后、被同一可执行文件链接时出现重复符号冲突。
add_library(cppmcp_common
    types.cpp
    jsonrpc.cpp
    logging.cpp
    request_executor.cpp
)

# 公共属性（include 目录 / 第三方依赖 / 编译定义）集中在 common 的 PUBLIC，
# server/client 通过 PUBLIC 依赖自动传递获得，无需各自重复声明。
# BUILD_INTERFACE：构建本仓库内目标时用源码树路径；
# INSTALL_INTERFACE：导出安装后，外部项目用相对于安装前缀的头文件路径。
target_include_directories(cppmcp_common PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(cppmcp_common PUBLIC
    nlohmann_json::nlohmann_json
    asio::asio
    llhttp::llhttp_static
)

target_compile_definitions(cppmcp_common PUBLIC ASIO_STANDALONE)

if(WIN32)
    target_compile_definitions(cppmcp_common PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX)
endif()

add_library(cppmcp_server
    server.cpp
    context.cpp
    stdio_transport.cpp
    http_transport.cpp
    local_pipe_transport.cpp
)

target_link_libraries(cppmcp_server PUBLIC cppmcp_common)

add_library(cppmcp_client
    client.cpp
    pending_request.cpp
    process.cpp
    stdio_client_transport.cpp
    http_client_transport.cpp
    local_pipe_client_transport.cpp
)

target_link_libraries(cppmcp_client PUBLIC cppmcp_common)

set_target_properties(cppmcp_common cppmcp_server cppmcp_client
    PROPERTIES POSITION_INDEPENDENT_CODE ON)

# High warning level for the library itself. C4819 (code page 936 vs UTF-8
# sources) is cosmetic on zh-CN Windows and excluded; system/third-party
# headers are already excluded via /external or MSVC default.
if(MSVC)
    target_compile_options(cppmcp_common PRIVATE /W4 /wd4819)
    target_compile_options(cppmcp_server PRIVATE /W4 /wd4819)
    target_compile_options(cppmcp_client PRIVATE /W4 /wd4819)
else()
    target_compile_options(cppmcp_common PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(cppmcp_server PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(cppmcp_client PRIVATE -Wall -Wextra -Wpedantic)
endif()
