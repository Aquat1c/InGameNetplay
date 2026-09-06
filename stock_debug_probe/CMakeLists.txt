# -----------------------------------------------------------------------------
# stock_debug_probe - ONE observe-only debugging DLL (output name: debug.dll)
# that packs multiple probes for studying STOCK EfzRevival behavior (no
# InGameNetplay). Each probe writes its OWN CSV under debug_logs\; status
# messages from every probe share one debug_status log + a live console. A
# shared host (probe_host.cpp) owns the single DllMain, waits for Revival once,
# does the single MH_Initialize, and drives every probe through a
# self-registration registry (see probe_common.h).
#
# Adding a probe = drop a src/<probe>.cpp that defines Install/Shutdown (and
# optionally Status) plus a ProbeRegistrar, then add it to PROBE_SOURCES below.
# The host logic is split from DllMain so this can later be reused from a
# standalone EXE main(), as the folder grows into a general EFZ debugging tool.
#
# Configure/build (XP-compatible Win32, mirrors the mod):
#   cmake -S stock_debug_probe -B build_probe -A Win32 -T v141_xp
#   cmake --build build_probe --config Release
# Output: build_probe/bin/<config>/debug.dll
# -----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.21)
project(stock_debug_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT so the probe DLL loads without a VC redist, matching the mod.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Locate vendored MinHook the same way the main project does: in-tree, else the
# sibling InGameControlsRebind checkout, else the parent mod's third_party.
set(_probe_minhook "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/minhook")
if(NOT EXISTS "${_probe_minhook}/CMakeLists.txt")
    set(_probe_minhook "${CMAKE_CURRENT_SOURCE_DIR}/../../InGameControlsRebind/third_party/minhook")
endif()
if(NOT EXISTS "${_probe_minhook}/CMakeLists.txt")
    message(FATAL_ERROR "stock_debug_probe requires MinHook source at ../third_party/minhook or ../../InGameControlsRebind/third_party/minhook")
endif()
if(NOT TARGET minhook)
    add_subdirectory("${_probe_minhook}" "${CMAKE_BINARY_DIR}/minhook" EXCLUDE_FROM_ALL)
endif()

# The single DLL: shared host + every probe module. Each probe hooks DISTINCT
# addresses, so all are active at once:
#   stock_rng_probe       - rand replacement (0x6E1A0) + efz effect proc (0x401C20)
#   stock_rollback_probe  - savestate save (0x6F390) + load (0x6F270)
#   stock_batch_probe     - rollback batch dispatcher (0x72D30)
#   stock_seed_probe      - minstd seed-apply (0x111D0)
#   stock_session_probe   - hook-free 4 Hz session timeline sampler
# Add a probe by appending its src/<probe>.cpp here.
set(PROBE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/probe_host.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/stock_rng_probe.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/stock_rollback_probe.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/stock_batch_probe.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/stock_seed_probe.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/stock_session_probe.cpp")

add_library(stock_debug_probe SHARED ${PROBE_SOURCES})
target_compile_definitions(stock_debug_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(stock_debug_probe PRIVATE
    "${_probe_minhook}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(stock_debug_probe PRIVATE minhook)
set_target_properties(stock_debug_probe PROPERTIES
    OUTPUT_NAME "debug"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
