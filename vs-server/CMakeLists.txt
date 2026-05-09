cmake_minimum_required(VERSION 3.16)

project(vs-server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

if(NOT WIN32)
  find_package(CURL REQUIRED)
  find_package(SQLite3 REQUIRED)
endif()

add_executable(vs-server
  src/main.cpp
  src/db/database.cpp
  src/db/sqlite_dynamic.cpp
  src/net/message_parse.cpp
  src/net/mail_helper_http.cpp
  src/net/file_relay.cpp
  src/net/tcp_server.cpp
  src/net/wire.cpp
  src/utils/logger.cpp
  src/utils/password_hash.cpp
  src/utils/base64.cpp
  src/utils/platform_paths.cpp
)

target_include_directories(vs-server PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/picosha2
)

target_link_libraries(vs-server PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(vs-server PRIVATE /utf-8)
endif()

if(WIN32)
  target_link_libraries(vs-server PRIVATE ws2_32 winhttp bcrypt)
else()
  target_link_libraries(vs-server PRIVATE CURL::libcurl SQLite::SQLite3)
endif()
