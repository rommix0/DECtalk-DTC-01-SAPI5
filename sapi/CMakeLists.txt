cmake_minimum_required(VERSION 3.15)
project(DectalkDtc01SAPI CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_compile_definitions(BUILD_X64)
endif()
add_library(dtc01common STATIC
  src/rom_images.cpp
  src/text_pipeline.cpp
  src/dtc01_core.cpp)
target_include_directories(dtc01common PUBLIC src)
target_link_libraries(dtc01common PUBLIC bcrypt)

add_executable(test_rom_images tools/test_rom_images.cpp)
target_link_libraries(test_rom_images PRIVATE dtc01common bcrypt)

add_executable(test_core tools/test_core.cpp)
target_link_libraries(test_core PRIVATE dtc01common bcrypt)

add_executable(test_voices tools/test_voices.cpp)
target_link_libraries(test_voices PRIVATE dtc01common)

add_executable(test_pipeline tools/test_pipeline.cpp)
target_link_libraries(test_pipeline PRIVATE dtc01common)
