add_executable(cppmcp_tests
    test_jsonrpc.cpp
    test_types.cpp
    test_server.cpp
    test_integration.cpp
    test_client.cpp
)

target_link_libraries(cppmcp_tests PRIVATE cppmcp_lib GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(cppmcp_tests)