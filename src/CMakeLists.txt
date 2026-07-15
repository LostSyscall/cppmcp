add_library(cppmcp_lib
    types.cpp
    jsonrpc.cpp
    context.cpp
    server.cpp
    stdio_transport.cpp
    http_transport.cpp
    local_pipe_transport.cpp
    logging.cpp
    request_executor.cpp
    client.cpp
    pending_request.cpp
    process.cpp
    stdio_client_transport.cpp
    local_pipe_client_transport.cpp
    http_client_transport.cpp
)

target_include_directories(cppmcp_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(cppmcp_lib PUBLIC
    nlohmann_json::nlohmann_json
    asio::asio
    llhttp::llhttp_static
)

target_compile_definitions(cppmcp_lib PUBLIC ASIO_STANDALONE)

if(WIN32)
    target_compile_definitions(cppmcp_lib PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX)
endif()