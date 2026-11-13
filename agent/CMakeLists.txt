# Agent library: parsers, collector, queue, agent threads, TCP client
add_library(sysmon_agent_core
  src/proc_parser.cpp
  src/collector.cpp
  src/agent.cpp
  src/tcp_client.cpp
)
target_include_directories(sysmon_agent_core PUBLIC include)
target_link_libraries(sysmon_agent_core PUBLIC sysmon_common Threads::Threads)

# Agent executable
add_executable(sysmon_agent src/main.cpp)
target_link_libraries(sysmon_agent PRIVATE sysmon_agent_core)

if(GTest_FOUND)
  add_executable(agent_tests
    tests/test_proc_parser.cpp
    tests/test_collector.cpp
    tests/test_thread_safe_queue.cpp
    tests/test_agent.cpp
  )
  target_link_libraries(agent_tests PRIVATE sysmon_agent_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(agent_tests)
endif()
