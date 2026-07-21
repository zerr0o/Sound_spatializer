cmake_minimum_required(VERSION 3.24)
project(SoundSpatializerDriverTransportTests LANGUAGES CXX)

include(CTest)
find_package(Threads REQUIRED)

add_executable(sound_spatializer_render_history_tests
    tests/RenderHistoryTests.cpp
)
target_compile_features(sound_spatializer_render_history_tests PRIVATE cxx_std_17)
target_include_directories(sound_spatializer_render_history_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../overlay"
)
target_link_libraries(sound_spatializer_render_history_tests PRIVATE Threads::Threads)

if(MSVC)
    target_compile_options(sound_spatializer_render_history_tests PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(sound_spatializer_render_history_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME sound_spatializer_render_history_integrity
         COMMAND sound_spatializer_render_history_tests)
