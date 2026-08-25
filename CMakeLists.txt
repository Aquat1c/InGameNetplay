cmake_minimum_required(VERSION 3.21)
project(EFZInGameNetplayMod VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

option(EFZ_STATIC_RUNTIME "Link MSVC runtime statically for single-DLL deployment" ON)
option(EFZ_XP_COMPAT "Enable Windows XP-oriented compile settings (requires compatible toolset)" OFF)
option(EFZ_EMBEDDED_TLS "Enable embedded TLS HTTPS backend using bundled mbedTLS" ON)
# Investigation builds only: compiles verbose lifecycle/per-frame logging.
# The invasive rollback/RNG observer was removed and is no longer part of any
# build flavor. OFF for end-user builds: trace call sites compile to nothing.
option(EFZ_LIFECYCLE_TRACE "Compile verbose lifecycle investigation logging (debug builds only)" OFF)
# Controlled A/B build: retain the init-time frame dispatcher hook but leave
# Revival's actual per-frame tick target untouched. This isolates the large
# OurPerFrameTickHook wrapper as a desync variable. It is intentionally not the
# shipping default because normal gameplay-exit recovery currently uses that
# wrapper; use the dedicated preset only for parity captures.
option(EFZ_NATIVE_TICK_PASSTHROUGH
    "Do not detour Revival's active-battle per-frame tick (diagnostic A/B)"
    OFF)

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
    src/netplay/bridge/external_launcher_guard.cpp
    src/netplay/bridge/frontend_return.cpp
    src/netplay/bridge/gameplay_exit_recovery.cpp
    src/netplay/bridge/session_bridge.cpp
    src/netplay/bridge/session_lifecycle.cpp
    src/netplay/bridge/revival_takeover.cpp
    src/netplay/bridge/netplay_state_export.cpp
    src/netplay/bridge/console_capture.cpp
    src/netplay/bridge/revival_memory.cpp
    src/netplay/bridge/revival_j_diagnostics.cpp
    src/netplay/bridge/revival_launcher_probe.cpp
    src/netplay/bridge/ipc_shared.cpp
    src/netplay/bridge/process_inject.cpp
    src/netplay/bridge/iat_stubs.cpp
    src/netplay/core/inline_edit.cpp
    src/netplay/interop/overlay_protocol.cpp
    src/netplay/interop/palette_store.cpp
    src/netplay/interop/palette_source.cpp
    src/netplay/interop/palette_remap.cpp
    src/netplay/interop/palette_apply.cpp
    src/netplay/interop/palette_socket.cpp
    src/netplay/interop/palette_charselect.cpp
    src/netplay/interop/overlay_channel.cpp
    src/netplay/core/input_utils.cpp
    src/netplay/core/lobby_client.cpp
    src/netplay/core/menu_model.cpp
    src/netplay/core/mod_settings.cpp
    src/netplay/core/network_capability.cpp
    src/netplay/core/network_endpoint.cpp
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
if(EFZ_NATIVE_TICK_PASSTHROUGH)
    target_compile_definitions(
        efz_netplay_mod PRIVATE EFZ_NATIVE_TICK_PASSTHROUGH=1)
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
    target_link_libraries(efz_netplay_mod PRIVATE advapi32)
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

set(EFZ_REVIVAL_102J_VERIFY_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_revival_102j.py")
if(Python3_Interpreter_FOUND)
    if(EXISTS "${EFZ_REVIVAL_102J_VERIFY_SCRIPT}")
        add_custom_target(verify_revival_102j
            COMMAND "${Python3_EXECUTABLE}" "${EFZ_REVIVAL_102J_VERIFY_SCRIPT}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Verifying EfzRevival 1.02j profile and hook contracts against raw binaries")
    else()
        message(WARNING
            "EfzRevival 1.02j verifier source is absent: "
            "${EFZ_REVIVAL_102J_VERIFY_SCRIPT}. The verify_revival_102j target "
            "will not be available in this checkout.")
    endif()
endif()

if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(
        NAME simulation_parity_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_simulation_parity.py"
            "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME revival_launcher_adoption_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_revival_launcher_adoption.py"
            "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME revival_compile_profile_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_revival_compile_profiles.py"
            "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME tournament_queue_admission_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_tournament_queue_admission.py"
            "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME managed_charselect_quit_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_managed_charselect_quit_contract.py"
            "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME rehost_status_reset_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_rehost_status_reset.py"
            "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME spectate_prompt_ack_latch_contract
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_spectate_prompt_ack_latch.py"
            "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(BUILD_TESTING)
    add_executable(revival_launch_policy_tests
        tests/revival_launch_policy_tests.cpp)
    target_include_directories(revival_launch_policy_tests PRIVATE include)
    target_compile_features(revival_launch_policy_tests PRIVATE cxx_std_17)

    if(MSVC)
        if(EFZ_XP_COMPAT)
            target_compile_options(revival_launch_policy_tests PRIVATE
                /W4 /EHsc)
            target_compile_definitions(revival_launch_policy_tests PRIVATE
                WINVER=0x0501 _WIN32_WINNT=0x0501 EFZ_XP_COMPAT=1)
            target_compile_options(revival_launch_policy_tests PRIVATE
                /Zc:threadSafeInit-)
        else()
            target_compile_options(revival_launch_policy_tests PRIVATE
                /W4 /permissive- /EHsc)
        endif()

        if(EFZ_STATIC_RUNTIME)
            set_property(TARGET revival_launch_policy_tests PROPERTY
                MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    endif()

    add_test(
        NAME revival_launch_policy_tests
        COMMAND revival_launch_policy_tests)
endif()

if(BUILD_TESTING)
    # Overlay-channel unit tests. The test file stubs mod::Log and
    # mod_settings::IsModInteropChannelEnabled, so only the interop sources are
    # linked (no ini/logging layers). GetTickCount pulls in kernel32 only.
    add_executable(interop_tests
        tests/interop_tests.cpp
        src/netplay/interop/overlay_protocol.cpp
        src/netplay/interop/palette_store.cpp
        src/netplay/interop/palette_source.cpp
        src/netplay/interop/palette_remap.cpp
        src/netplay/interop/overlay_channel.cpp)
    target_include_directories(interop_tests PRIVATE include)
    target_compile_features(interop_tests PRIVATE cxx_std_17)
    # The test writes temp .pal fixtures with the CRT (fopen); production code
    # uses the Win32 API. Suppress the CRT-deprecation warning for the test only.
    target_compile_definitions(interop_tests PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)

    if(MSVC)
        if(EFZ_XP_COMPAT)
            target_compile_options(interop_tests PRIVATE /W4 /EHsc)
            target_compile_definitions(interop_tests PRIVATE
                WINVER=0x0501 _WIN32_WINNT=0x0501 EFZ_XP_COMPAT=1)
            target_compile_options(interop_tests PRIVATE
                /Zc:threadSafeInit-)
        else()
            target_compile_options(interop_tests PRIVATE
                /W4 /permissive- /EHsc)
        endif()

        if(EFZ_STATIC_RUNTIME)
            set_property(TARGET interop_tests PROPERTY
                MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    endif()

    add_test(NAME interop_tests COMMAND interop_tests)
endif()

if(BUILD_TESTING AND WIN32)
    add_executable(network_endpoint_tests EXCLUDE_FROM_ALL
        tests/network_endpoint_tests.cpp
        src/netplay/core/network_capability.cpp
        src/netplay/core/network_endpoint.cpp)
    target_include_directories(network_endpoint_tests PRIVATE include)
    target_compile_features(network_endpoint_tests PRIVATE cxx_std_17)
    target_compile_definitions(network_endpoint_tests PRIVATE NOMINMAX)
    target_link_libraries(network_endpoint_tests PRIVATE ws2_32)

    if(MSVC)
        if(EFZ_XP_COMPAT)
            target_compile_options(network_endpoint_tests PRIVATE /W4 /EHsc)
            target_compile_definitions(network_endpoint_tests PRIVATE
                WINVER=0x0501 _WIN32_WINNT=0x0501 EFZ_XP_COMPAT=1)
            target_compile_options(network_endpoint_tests PRIVATE
                /Zc:threadSafeInit-)
        else()
            target_compile_options(network_endpoint_tests PRIVATE
                /W4 /permissive- /EHsc)
        endif()

        if(EFZ_STATIC_RUNTIME)
            set_property(TARGET network_endpoint_tests PROPERTY
                MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    endif()

    add_test(NAME network_endpoint_tests COMMAND network_endpoint_tests)

    add_executable(console_handoff_contract_tests EXCLUDE_FROM_ALL
        tests/console_handoff_contract_tests.cpp)
    target_include_directories(console_handoff_contract_tests PRIVATE include)
    target_compile_features(console_handoff_contract_tests PRIVATE cxx_std_17)
    target_compile_definitions(console_handoff_contract_tests PRIVATE NOMINMAX)

    if(MSVC)
        if(EFZ_XP_COMPAT)
            target_compile_options(console_handoff_contract_tests PRIVATE
                /W4 /EHsc)
            target_compile_definitions(console_handoff_contract_tests PRIVATE
                WINVER=0x0501 _WIN32_WINNT=0x0501 EFZ_XP_COMPAT=1)
            target_compile_options(console_handoff_contract_tests PRIVATE
                /Zc:threadSafeInit-)
        else()
            target_compile_options(console_handoff_contract_tests PRIVATE
                /W4 /permissive- /EHsc)
        endif()

        if(EFZ_STATIC_RUNTIME)
            set_property(TARGET console_handoff_contract_tests PROPERTY
                MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    endif()

    add_test(
        NAME console_handoff_contract_tests
        COMMAND console_handoff_contract_tests)
endif()
