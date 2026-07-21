cmake_minimum_required(VERSION 3.21)
project(EFZInGameNetplayMod VERSION 0.1.0 LANGUAGES C CXX)

option(EFZ_STATIC_RUNTIME "Link MSVC runtime statically for single-DLL deployment" ON)
option(EFZ_XP_COMPAT "Enable Windows XP-oriented compile settings (requires compatible toolset)" OFF)
option(EFZ_EMBEDDED_TLS "Enable embedded TLS HTTPS backend using bundled mbedTLS" ON)
# Investigation builds only: compiles the lifecycle/desync diagnostic layer
# (MOD_LIFECYCLE_TRACE call sites, verbose per-frame dumps, forensic tracers
# defaulting ON) into the binary.  OFF for end-user builds: the trace calls
# compile to nothing and the forensic INI keys default OFF.
option(EFZ_LIFECYCLE_TRACE "Compile in lifecycle/desync investigation tracing (debug builds only)" OFF)

if(EFZ_XP_COMPAT AND NOT EFZ_EMBEDDED_TLS)
    message(FATAL_ERROR
        "EFZ_XP_COMPAT requires EFZ_EMBEDDED_TLS: XP's SChannel backends cannot reliably connect to the modern Concerto HTTPS endpoint.")
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)

if(MSVC AND EFZ_STATIC_RUNTIME)
    # Apply static CRT globally so third-party static libraries (mbedTLS) use the same runtime model.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

if(EFZ_EMBEDDED_TLS)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/CMakeLists.txt")
        message(FATAL_ERROR "EFZ_EMBEDDED_TLS is ON but third_party/mbedtls is missing.")
    endif()

    # Required for CMake >= 4 when consuming mbedTLS 2.x's older minimum version declaration.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)

    add_subdirectory(third_party/mbedtls EXCLUDE_FROM_ALL)
endif()

add_library(efz_netplay_mod SHARED
    src/crash_handler.cpp
    src/dllmain.cpp
    src/logger.cpp
    src/netplay_mod.rc
    src/netplay/assets/assets.cpp
    src/netplay/bridge/async_hosting.cpp
    src/netplay/bridge/frontend_return.cpp
    src/netplay/bridge/gameplay_exit_recovery.cpp
    src/netplay/bridge/session_bridge.cpp
    src/netplay/bridge/session_lifecycle.cpp
    src/netplay/bridge/revival_takeover.cpp
    src/netplay/bridge/netplay_state_export.cpp
    src/netplay/bridge/console_capture.cpp
    src/netplay/bridge/desync_monitor.cpp
    src/netplay/bridge/batch_stabilizer.cpp
    src/netplay/bridge/revival_memory.cpp
    src/netplay/bridge/revival_j_diagnostics.cpp
    src/netplay/bridge/ipc_shared.cpp
    src/netplay/bridge/process_inject.cpp
    src/netplay/bridge/iat_stubs.cpp
    src/netplay/core/inline_edit.cpp
    src/netplay/core/input_utils.cpp
    src/netplay/core/lobby_client.cpp
    src/netplay/core/menu_model.cpp
    src/netplay/core/mod_settings.cpp
    src/netplay/core/options_keybinds.cpp
    src/netplay/core/patch_utils.cpp
    src/netplay/core/tls_http_client.cpp
    src/netplay/core/text_utils.cpp
    src/netplay/core/validation.cpp
    src/netplay/hooks/menu_hooks.cpp
    src/netplay/hooks/battle_log_menu.cpp
    src/netplay/hooks/options_menu.cpp
    src/netplay/hooks/player_rooms_menu.cpp
    src/netplay/hooks/title_assets.cpp
    src/netplay/hooks/title_core.cpp
    src/netplay/hooks/title_draw_layers.cpp
    src/netplay/hooks/title_flow.cpp
    src/netplay/hooks/title_overlay_text.cpp
    src/netplay/hooks/title_patch_helpers.cpp
    src/netplay/hooks/title_patch_install.cpp
    src/netplay/render/draw_surface.cpp
    src/netplay/render/menu_overlay.cpp
    src/netplay/render/software_font.cpp
    src/netplay/render/sprite_font_map.cpp
    src/netplay/hooks/debug_overlay.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx9.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
)

target_include_directories(efz_netplay_mod PRIVATE include)
target_include_directories(efz_netplay_mod PRIVATE
    third_party/imgui
    third_party/imgui/backends)
target_compile_features(efz_netplay_mod PRIVATE cxx_std_17)
target_compile_definitions(efz_netplay_mod PRIVATE NOMINMAX)
if(EFZ_LIFECYCLE_TRACE)
    target_compile_definitions(efz_netplay_mod PRIVATE EFZ_LIFECYCLE_TRACE=1)
endif()

if(EFZ_EMBEDDED_TLS)
    target_compile_definitions(efz_netplay_mod PRIVATE EFZ_EMBEDDED_TLS=1)
    target_link_libraries(efz_netplay_mod PRIVATE mbedtls mbedx509 mbedcrypto)
endif()

set(EFZ_MINHOOK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook")
if(NOT EXISTS "${EFZ_MINHOOK_SOURCE_DIR}/CMakeLists.txt")
    set(EFZ_MINHOOK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../InGameControlsRebind/third_party/minhook")
endif()

if(WIN32)
    if(NOT EXISTS "${EFZ_MINHOOK_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "Battle Log D3D9 overlay requires MinHook source at third_party/minhook or ../InGameControlsRebind/third_party/minhook")
    endif()
    if(NOT TARGET minhook)
        add_subdirectory("${EFZ_MINHOOK_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/third_party/minhook" EXCLUDE_FROM_ALL)
    endif()
    target_link_libraries(efz_netplay_mod PRIVATE winmm)
    target_link_libraries(efz_netplay_mod PRIVATE ws2_32)
    target_link_libraries(efz_netplay_mod PRIVATE gdiplus)
    target_link_libraries(efz_netplay_mod PRIVATE d3d9)
    target_link_libraries(efz_netplay_mod PRIVATE minhook)
endif()

if(MSVC)
    # Emit a linker .map (and keep symbol RVAs) so crash return addresses - which
    # EFZ Revival's crash handler dumps to logEfz.txt as "return to 0xADDR" - can
    # be symbolized to function + offset (RVA = addr - module load base).
    target_link_options(efz_netplay_mod PRIVATE /MAP /MAPINFO:EXPORTS)
    if(EFZ_XP_COMPAT)
        # The Windows 7.1A SDK shipped with the v141_xp toolset has headers
        # that do not compile under /permissive-.  Use relaxed conformance
        # mode so the old SDK headers are accepted.
        target_compile_options(efz_netplay_mod PRIVATE /W4 /EHsc)
        target_compile_definitions(efz_netplay_mod PRIVATE
            WINVER=0x0501 _WIN32_WINNT=0x0501 EFZ_XP_COMPAT=1)
        target_compile_options(efz_netplay_mod PRIVATE /Zc:threadSafeInit-)
        if(NOT CMAKE_GENERATOR_TOOLSET MATCHES "v141_xp")
            message(WARNING "EFZ_XP_COMPAT is enabled but generator toolset is not v141_xp; the binary may still import Vista+ APIs.")
        endif()
    else()
        target_compile_options(efz_netplay_mod PRIVATE /W4 /permissive- /EHsc)
    endif()

    if(EFZ_STATIC_RUNTIME)
        set_property(TARGET efz_netplay_mod PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

set_target_properties(efz_netplay_mod PROPERTIES
    OUTPUT_NAME "efz_netplay_mod"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(Python3_Interpreter_FOUND)
    add_custom_target(verify_revival_102j
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_revival_102j.py"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Verifying EfzRevival 1.02j profile and hook contracts against raw binaries")
endif()
