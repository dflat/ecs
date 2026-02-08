cmake_minimum_required(VERSION 3.14)
project(ecs LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(ecs INTERFACE)
target_include_directories(ecs INTERFACE include)
target_compile_features(ecs INTERFACE cxx_std_17)

add_executable(ecs_test tests/main.cpp)
target_link_libraries(ecs_test PRIVATE ecs)
target_compile_options(ecs_test PRIVATE -Wall -Wextra -Wpedantic)
