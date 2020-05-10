cmake_minimum_required(VERSION 3.10)
project(file-server)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS_DEBUG "-fsanitize=address ${CMAKE_CXX_FLAGS_DEBUG}")
add_executable(server server.cpp common.cpp)
add_executable(client client.cpp common.cpp)