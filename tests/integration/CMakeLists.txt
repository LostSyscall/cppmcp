add_executable(cppmcp_integration_tests
    test_helpers.cpp
    test_local_pipe_transport.cpp
    test_streamable_http_transport.cpp
    test_http_sse_transport.cpp
    test_stdio_transport.cpp
    test_client_stdio.cpp
    test_client_http.cpp
)

target_link_libraries(cppmcp_integration_tests PRIVATE
    cppmcp_server
    cppmcp_client
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(cppmcp_integration_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Pass stdio server executable path as compile definition
target_compile_definitions(cppmcp_integration_tests PRIVATE
    CPPMCP_STDIO_SERVER_PATH="$<TARGET_FILE:cppmcp_test_stdio_server>"
)

# Stdio server subprocess (separate executable for child process testing)
add_executable(cppmcp_test_stdio_server
    test_stdio_server_main.cpp
)

target_link_libraries(cppmcp_test_stdio_server PRIVATE
    cppmcp_server
)

include(GoogleTest)
# TIMEOUT bounds each test: a hung child process / dead socket must fail the
# suite instead of stalling CI forever.
gtest_discover_tests(cppmcp_integration_tests
    PROPERTIES LABELS "integration" TIMEOUT 120
)