cmake_minimum_required(VERSION 3.16)
project(PersonalCloudBackup LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(backup_core src/core/backup_core.cpp)

if(UNIX)
  find_package(Threads REQUIRED)
  add_executable(backup_server src/server/backup_server.cpp)
  target_link_libraries(backup_server PRIVATE Threads::Threads)
endif()
