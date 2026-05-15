add_library(cppmcp_lib
    types.cpp
    jsonrpc.cpp
    context.cpp
    server.cpp
    stdio_transport.cpp
    http_transport.cpp
    local_pipe_transport.cpp
)

target_include_directories(cppmcp_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(cppmcp_lib PUBLIC
    nlohmann_json::nlohmann_json
    httplib::httplib
)

if(WIN32)
    target_compile_definitions(cppmcp_lib PUBLIC NOMINMAX)
endif()