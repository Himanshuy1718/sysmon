add_library(sysmon_server_core
  src/node_registry.cpp
  src/server.cpp
)
target_include_directories(sysmon_server_core PUBLIC include)
target_link_libraries(sysmon_server_core PUBLIC sysmon_common Threads::Threads)

add_executable(sysmon_server src/main.cpp)
target_link_libraries(sysmon_server PRIVATE sysmon_server_core)

if(GTest_FOUND)
  # Network tests use both the server and the agent's TcpClient.
  add_executable(network_tests
    tests/test_protocol.cpp
    tests/test_socket.cpp
    tests/test_node_registry.cpp
    tests/test_end_to_end.cpp
  )
  target_link_libraries(network_tests PRIVATE sysmon_server_core sysmon_agent_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(network_tests)
endif()
