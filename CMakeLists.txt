cmake_minimum_required(VERSION 3.16)
project(sysmon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

enable_testing()

find_package(Threads REQUIRED)
find_package(GTest QUIET)
if(NOT GTest_FOUND)
  message(WARNING "GoogleTest not found - tests will not be built. Install with: sudo apt install libgtest-dev")
endif()

# Optional: build with ThreadSanitizer to detect data races.
#   cmake -S . -B build-tsan -DSYSMON_TSAN=ON
option(SYSMON_TSAN "Build with ThreadSanitizer" OFF)
if(SYSMON_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()

add_subdirectory(common)   # shared: data types, sockets, protocol
add_subdirectory(agent)    # runs on every monitored machine
add_subdirectory(server)   # central collector
