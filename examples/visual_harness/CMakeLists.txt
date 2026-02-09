add_executable(visual_harness main.cpp)
target_link_libraries(visual_harness PRIVATE ecs raylib)
target_compile_features(visual_harness PRIVATE cxx_std_17)
target_compile_options(visual_harness PRIVATE -Wall -Wextra -Wpedantic)
