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
  src/dtc01_core.cpp
  src/registry.cpp
  src/com.cpp
  src/DectalkTtsEngine.cpp
  src/ratebooster.cpp)
target_include_directories(dtc01common PUBLIC src)
# sapi/ole32/oleaut32 come in with DectalkTtsEngine.cpp (ISpTTSEngine +
# CoTaskMemAlloc); propagated so the engine's tests/probes link them too.
target_link_libraries(dtc01common PUBLIC bcrypt advapi32 sapi ole32 oleaut32)

# Test/tool executables must keep assert() live even in Release, where MSVC
# would otherwise define NDEBUG and compile asserts to no-ops. /UNDEBUG tells
# MSVC to undefine NDEBUG; CMake appends target-level compile options after
# the per-config flags, so this wins over the Release /DNDEBUG.
function(add_dtc_tool name)
  add_executable(${name} tools/${name}.cpp)
  target_link_libraries(${name} PRIVATE dtc01common ${ARGN})
  target_compile_options(${name} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
endfunction()

add_dtc_tool(test_rom_images bcrypt)
add_dtc_tool(test_core bcrypt)
add_dtc_tool(test_voices)
add_dtc_tool(test_pipeline)
add_dtc_tool(dtc01_speak)
add_dtc_tool(test_tokens advapi32)
add_dtc_tool(test_outputformat sapi ole32 oleaut32 advapi32)
add_dtc_tool(test_exports)
add_dtc_tool(test_settings advapi32)
add_dtc_tool(test_ratebooster)

# sapi_probe drives the real DectalkDtc01SAPI.dll coclass end-to-end through
# DllGetClassObject (no registration); it links dtc01common like every other
# tool here but doesn't use any of its symbols -- it builds its own in-memory
# ISpObjectToken stand-in and WAV writer (see tools/sapi_probe.cpp).
add_dtc_tool(sapi_probe sapi ole32 oleaut32 advapi32)

# The in-proc COM server: DectalkTtsEngine.cpp and com.cpp already live in
# dtc01common (linked below), so the DLL target itself is just the four
# exported entry points in sapi_main.cpp plus the .def that exports them --
# nothing here is compiled a second time, avoiding duplicate-symbol errors
# against dtc01common or the test targets above that also link it.
add_library(DectalkDtc01SAPI SHARED src/sapi_main.cpp)
target_link_libraries(DectalkDtc01SAPI PRIVATE dtc01common)
set_target_properties(DectalkDtc01SAPI PROPERTIES
  OUTPUT_NAME "DectalkDtc01SAPI"
  LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/dectalk_sapi.def")
