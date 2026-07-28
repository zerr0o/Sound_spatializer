cmake_minimum_required(VERSION 3.24)

project(SoundSpatializerEngine VERSION 0.2.1 LANGUAGES CXX)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(SOUND_SPATIALIZER_BUILD_TESTS "Build the engine's dependency-free tests" ON)
option(SOUND_SPATIALIZER_ENABLE_MYSOFA "Enable optional libmysofa support when available" ON)

add_library(sound_spatializer_engine STATIC
    src/acoustics.cpp
    src/audio_backend_mock.cpp
    src/config.cpp
    src/dsp.cpp
    src/engine.cpp
    src/hrtf.cpp
    src/hrtf_worker.cpp
    src/ipc.cpp
    src/pose.cpp
    src/spectral.cpp
    src/window_audio.cpp
)

target_compile_features(sound_spatializer_engine PUBLIC cxx_std_20)
target_include_directories(sound_spatializer_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(sound_spatializer_engine PUBLIC
    SOUND_SPATIALIZER_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    SOUND_SPATIALIZER_VERSION_MINOR=${PROJECT_VERSION_MINOR}
)

if(MSVC)
    # C4324 is the expected cache-line/SIMD padding introduced by alignas; all
    # other project warnings remain fatal in the Windows production build.
    target_compile_options(sound_spatializer_engine PRIVATE /W4 /WX /wd4324 /permissive- /EHsc /utf-8)
else()
    target_compile_options(sound_spatializer_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_sources(sound_spatializer_engine PRIVATE src/audio_backend_wasapi.cpp)
    target_include_directories(sound_spatializer_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../driver/include)
    target_compile_definitions(sound_spatializer_engine PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(sound_spatializer_engine PRIVATE
        ole32
        avrt
        advapi32
        Mmdevapi
        dwmapi
    )
endif()

if(SOUND_SPATIALIZER_ENABLE_MYSOFA)
    set(SOUND_SPATIALIZER_MYSOFA_FOUND OFF)
    find_package(mysofa CONFIG QUIET)
    if(TARGET mysofa::mysofa-shared OR TARGET mysofa::mysofa-static)
        target_sources(sound_spatializer_engine PRIVATE src/hrtf_mysofa.cpp)
        if(TARGET mysofa::mysofa-shared)
            target_link_libraries(sound_spatializer_engine PRIVATE mysofa::mysofa-shared)
        else()
            target_link_libraries(sound_spatializer_engine PRIVATE mysofa::mysofa-static)
        endif()
        target_compile_definitions(sound_spatializer_engine PUBLIC SOUND_SPATIALIZER_HAS_MYSOFA=1)
        set(SOUND_SPATIALIZER_MYSOFA_FOUND ON)
    else()
        find_path(MYSOFA_INCLUDE_DIR mysofa.h)
        find_library(MYSOFA_LIBRARY NAMES mysofa mysofa_shared mysofa_static libmysofa)
        if(MYSOFA_INCLUDE_DIR AND MYSOFA_LIBRARY)
            target_sources(sound_spatializer_engine PRIVATE src/hrtf_mysofa.cpp)
            target_include_directories(sound_spatializer_engine PRIVATE ${MYSOFA_INCLUDE_DIR})
            target_link_libraries(sound_spatializer_engine PRIVATE ${MYSOFA_LIBRARY})
            target_compile_definitions(sound_spatializer_engine PUBLIC SOUND_SPATIALIZER_HAS_MYSOFA=1)
            set(SOUND_SPATIALIZER_MYSOFA_FOUND ON)
        endif()
    endif()
    if(NOT SOUND_SPATIALIZER_MYSOFA_FOUND)
        message(FATAL_ERROR
            "SOUND_SPATIALIZER_ENABLE_MYSOFA=ON but libmysofa was not found. "
            "Install the dependency for a distributable build, or explicitly configure "
            "-DSOUND_SPATIALIZER_ENABLE_MYSOFA=OFF for the analytic development backend.")
    endif()
endif()

if(WIN32)
    add_executable(SoundSpatializer.Engine WIN32 app/main.cpp)
else()
    add_executable(SoundSpatializer.Engine app/main.cpp)
endif()
target_link_libraries(SoundSpatializer.Engine PRIVATE sound_spatializer_engine)
if(WIN32)
    target_link_libraries(SoundSpatializer.Engine PRIVATE shell32)
endif()

include(CTest)
if(BUILD_TESTING AND SOUND_SPATIALIZER_BUILD_TESTS)
    add_executable(sound_spatializer_engine_tests tests/test_main.cpp)
    target_link_libraries(sound_spatializer_engine_tests PRIVATE sound_spatializer_engine)
    target_compile_definitions(sound_spatializer_engine_tests PRIVATE
        SOUND_SPATIALIZER_TEST_REPOSITORY_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../..")
    add_test(NAME sound_spatializer_engine_tests COMMAND sound_spatializer_engine_tests)
endif()
