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
add_dtc_tool(test_config_persist advapi32)
add_dtc_tool(test_log advapi32)
add_dtc_tool(dump_pipeline)

# sapi_probe drives the real DectalkDtc01SAPI.dll coclass end-to-end through
# DllGetClassObject (no registration); it links dtc01common like every other
# tool here but doesn't use any of its symbols -- it builds its own in-memory
# ISpObjectToken stand-in and WAV writer (see tools/sapi_probe.cpp).
add_dtc_tool(sapi_probe sapi ole32 oleaut32 advapi32)

# cancel_probe (Task E2): drives the real DectalkDtc01SAPI.dll coclass through
# repeated mid-stream SPVES_ABORT cancellations (a soak for the real cancel
# path a SAPI host takes) -- same DllGetClassObject/no-registration load as
# sapi_probe above (see tools/cancel_probe.cpp).
add_dtc_tool(cancel_probe sapi ole32 oleaut32 advapi32)

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

# DectalkConfig: the accessible configuration utility (Task C4). A GUI
# (WIN32) executable rather than a console tool, so it needs its own
# add_executable rather than add_dtc_tool -- wWinMain requires the WIN32
# subsystem, and it has a .rc dialog resource to compile alongside its .cpp.
# It links dtc01common for voices.hpp/user_settings.hpp/voice_registry.hpp
# (all header-only) plus registry.cpp's out-of-line RegKey implementation;
# user32/comctl32 are for the dialog and its trackbars, ole32/oleaut32/sapi
# for "Play sample" (already PUBLIC on dtc01common, but named here too since
# the task brief calls them out explicitly as this target's own needs).
add_executable(DectalkConfig WIN32 tools/dectalk_config.cpp tools/dectalk_config.rc)
target_link_libraries(DectalkConfig PRIVATE dtc01common user32 comctl32 advapi32 ole32 oleaut32 sapi)

# DectalkDiagnostics (Task D2): drives the real DectalkDtc01SAPI.dll through
# DllGetClassObject for all 18 voice tokens and reports pass/fail -- an
# install-time smoke test. Links dtc01common for dtc01::VOICES (voices.hpp);
# not add_dtc_tool because its output name must be exactly
# DectalkDiagnostics.exe (add_dtc_tool infers the name from tools/<name>.cpp,
# which would give dectalk_diag.exe here).
add_executable(DectalkDiagnostics tools/dectalk_diag.cpp)
target_link_libraries(DectalkDiagnostics PRIVATE dtc01common sapi ole32 oleaut32 advapi32)
target_compile_options(DectalkDiagnostics PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
