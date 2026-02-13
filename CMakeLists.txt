cmake_minimum_required(VERSION 3.14)
project(ecs LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Options ---
option(ECS_BUILD_MODULES "Build standard modules (transform, hierarchy) with dependencies" ON)
option(ECS_SANITIZE "Enable ASan + UBSan" OFF)
option(ECS_BUILD_EXAMPLES "Build example programs" OFF)

# --- Target: Core (The Kernel) ---
add_library(ecs_core INTERFACE)
add_library(ecs::core ALIAS ecs_core)
target_include_directories(ecs_core INTERFACE include)
target_compile_features(ecs_core INTERFACE cxx_std_17)

# --- Target: Main Bundle (Batteries Included) ---
add_library(ecs INTERFACE)
target_link_libraries(ecs INTERFACE ecs_core)

# --- Modules ---
if(ECS_BUILD_MODULES)
    # 1. Transform Module (Depends on GLM)
    include(FetchContent)
    
    # Check if GLM is already available (e.g. from parent project)
    if(NOT TARGET glm::glm)
        message(STATUS "ECS: Fetching GLM for Transform module...")
        FetchContent_Declare(
            glm
            GIT_REPOSITORY https://github.com/g-truc/glm.git
            GIT_TAG        1.0.1
        )
        FetchContent_MakeAvailable(glm)
    endif()

    add_library(ecs_transform INTERFACE)
    add_library(ecs::transform ALIAS ecs_transform)
    target_link_libraries(ecs_transform INTERFACE ecs_core glm::glm)
    
    # Add transform module to the main 'ecs' bundle
    target_link_libraries(ecs INTERFACE ecs_transform)
endif()

# --- Development / Testing ---

add_executable(ecs_test tests/main.cpp)
# Link against the main bundle so tests verify full functionality
target_link_libraries(ecs_test PRIVATE ecs)
target_compile_options(ecs_test PRIVATE -Wall -Wextra -Wpedantic)

if(ECS_SANITIZE)
    target_compile_options(ecs_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(ecs_test PRIVATE -fsanitize=address,undefined)
endif()

if(ECS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
