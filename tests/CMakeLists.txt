add_executable(cppmcp_tests
    test_jsonrpc.cpp
    test_types.cpp
    test_server.cpp
    test_integration.cpp
    test_client.cpp
    test_protocol_conformance.cpp
)

target_link_libraries(cppmcp_tests PRIVATE cppmcp_server cppmcp_client GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(cppmcp_tests PROPERTIES TIMEOUT 60)