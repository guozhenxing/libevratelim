cmake_minimum_required(VERSION 2.8)
project(libevratelim)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
add_subdirectory(src)

message("Build Type: ${CMAKE_BUILD_TYPE}")

add_executable(test test/test.c)
target_link_libraries(test libevratelim event)
