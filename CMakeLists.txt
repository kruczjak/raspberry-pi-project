cmake_minimum_required(VERSION 3.6)
project(raspberry_pi_project)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

set(SOURCE_FILES http_server/server.c)

add_executable(raspberry_pi_project ${SOURCE_FILES})