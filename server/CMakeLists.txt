find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQ REQUIRED IMPORTED_TARGET libpq)  # sudo apt install libpq-dev

# Embed sql/schema.sql into the binary as a C++ string, so the server can
# create its tables on startup and the .sql file stays the single source of truth.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/sql/schema.sql SYSMON_SCHEMA_SQL)
configure_file(sql/schema_sql.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/sysmon/schema_sql.hpp @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS sql/schema.sql)

add_library(sysmon_server_core
  src/node_registry.cpp
  src/server.cpp
  src/db_writer.cpp
  src/pg_store.cpp
)
target_include_directories(sysmon_server_core
  PUBLIC include
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(sysmon_server_core PUBLIC sysmon_common Threads::Threads PRIVATE PkgConfig::LIBPQ)

add_executable(sysmon_server src/main.cpp)
target_link_libraries(sysmon_server PRIVATE sysmon_server_core)

if(GTest_FOUND)
  add_executable(network_tests
    tests/test_protocol.cpp
    tests/test_socket.cpp
    tests/test_node_registry.cpp
    tests/test_end_to_end.cpp
  )
  target_link_libraries(network_tests PRIVATE sysmon_server_core sysmon_agent_core GTest::gtest_main)

  # Storage tests. The PostgreSQL ones run only when SYSMON_TEST_DB is set
  # to a connection string; otherwise they are skipped.
  add_executable(storage_tests
    tests/test_db_writer.cpp
    tests/test_pg_store.cpp
  )
  target_link_libraries(storage_tests PRIVATE sysmon_server_core GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(network_tests)
  gtest_discover_tests(storage_tests)
endif()
