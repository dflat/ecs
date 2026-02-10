add_executable(stress_harness main.cpp)
target_link_libraries(stress_harness PRIVATE ecs raylib)
target_compile_features(stress_harness PRIVATE cxx_std_17)
target_compile_options(stress_harness PRIVATE -Wall -Wextra -Wpedantic)
