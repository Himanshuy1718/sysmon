find_package(Threads REQUIRED)

# Optional: build with ThreadSanitizer to detect data races.
#   cmake -S . -B build-tsan -DSYSMON_TSAN=ON
option(SYSMON_TSAN "Build with ThreadSanitizer" OFF)
if(SYSMON_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()

# Core library: parsers, collector, queue, agent (shared by binary and tests)
add_library(sysmon_core
  src/proc_parser.cpp
  src/collector.cpp
  src/agent.cpp
)
target_include_directories(sysmon_core PUBLIC include)
target_link_libraries(sysmon_core PUBLIC Threads::Threads)

# Agent executable
add_executable(sysmon_agent src/main.cpp)
target_link_libraries(sysmon_agent PRIVATE sysmon_core)

# Unit tests (GoogleTest)
find_package(GTest QUIET)
if(GTest_FOUND)
  add_executable(agent_tests
    tests/test_proc_parser.cpp
    tests/test_collector.cpp
    tests/test_thread_safe_queue.cpp
    tests/test_agent.cpp
  )
  target_link_libraries(agent_tests PRIVATE sysmon_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(agent_tests)
else()
  message(WARNING "GoogleTest not found - tests will not be built. Install with: sudo apt install libgtest-dev")
endif()
