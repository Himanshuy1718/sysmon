# Core library: parsers + collector (shared by the agent binary and the tests)
add_library(sysmon_core
  src/proc_parser.cpp
  src/collector.cpp
)
target_include_directories(sysmon_core PUBLIC include)

# Agent executable
add_executable(sysmon_agent src/main.cpp)
target_link_libraries(sysmon_agent PRIVATE sysmon_core)

# Unit tests (GoogleTest)
find_package(GTest QUIET)
if(GTest_FOUND)
  add_executable(agent_tests
    tests/test_proc_parser.cpp
    tests/test_collector.cpp
  )
  target_link_libraries(agent_tests PRIVATE sysmon_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(agent_tests)
else()
  message(WARNING "GoogleTest not found - tests will not be built. Install with: sudo apt install libgtest-dev")
endif()
