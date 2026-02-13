cmake_minimum_required(VERSION 3.14)
project(ecs LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Dependencies ---
# Always fetch GLM to ensure reproducible builds.
include(FetchContent)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
)
FetchContent_MakeAvailable(glm)

# --- Library ---
add_library(ecs INTERFACE)
target_include_directories(ecs INTERFACE include)
target_compile_features(ecs INTERFACE cxx_std_17)

# Expose GLM to consumers of ECS modules
target_link_libraries(ecs INTERFACE glm::glm)

# --- Development / Testing ---
option(ECS_SANITIZE "Enable ASan + UBSan" OFF)

add_executable(ecs_test tests/main.cpp)
target_link_libraries(ecs_test PRIVATE ecs)
target_compile_options(ecs_test PRIVATE -Wall -Wextra -Wpedantic)

if(ECS_SANITIZE)
    target_compile_options(ecs_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(ecs_test PRIVATE -fsanitize=address,undefined)
endif()

option(ECS_BUILD_EXAMPLES "Build example programs" OFF)
if(ECS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
