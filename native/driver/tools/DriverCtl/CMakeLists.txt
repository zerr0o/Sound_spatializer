cmake_minimum_required(VERSION 3.24)
project(SoundSpatializerDriverCtl LANGUAGES CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT WIN32)
    message(FATAL_ERROR "SoundSpatializer.DriverCtl is Windows-only")
endif()

add_executable(SoundSpatializer.DriverCtl main.cpp)
target_compile_features(SoundSpatializer.DriverCtl PRIVATE cxx_std_20)
target_include_directories(SoundSpatializer.DriverCtl PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../include")
target_compile_definitions(SoundSpatializer.DriverCtl PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(SoundSpatializer.DriverCtl PRIVATE Setupapi Newdev)

if(MSVC)
    target_compile_options(SoundSpatializer.DriverCtl PRIVATE /W4 /WX /permissive- /guard:cf)
    target_link_options(SoundSpatializer.DriverCtl PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
endif()
