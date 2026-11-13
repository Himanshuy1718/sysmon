# Code shared by the agent and the server: data types, sockets, protocol.
find_package(nlohmann_json 3 REQUIRED)  # sudo apt install nlohmann-json3-dev

add_library(sysmon_common
  src/socket.cpp
  src/protocol.cpp
)
target_include_directories(sysmon_common PUBLIC include)
# PRIVATE: only protocol.cpp sees the JSON library; users of the header don't.
target_link_libraries(sysmon_common PRIVATE nlohmann_json::nlohmann_json)
