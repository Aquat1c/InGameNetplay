cmake_minimum_required(VERSION 3.21)
project(EFZInGameNetplayMod VERSION 0.1.0 LANGUAGES C CXX)

option(EFZ_STATIC_RUNTIME "Link MSVC runtime statically for single-DLL deployment" ON)
option(EFZ_XP_COMPAT "Enable Windows XP-oriented compile settings (requires compatible toolset)" OFF)
option(EFZ_EMBEDDED_TLS "Enable embedded TLS HTTPS backend using bundled mbedTLS" ON)

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
    src/netplay/assets/assets.cpp
    src/netplay/bridge/session_bridge.cpp
    src/netplay/bridge/revival_takeover.cpp
    src/netplay/core/inline_edit.cpp
    src/netplay/core/input_utils.cpp
    src/netplay/core/lobby_client.cpp
    src/netplay/core/menu_model.cpp
    src/netplay/core/patch_utils.cpp
    src/netplay/core/tls_http_client.cpp
    src/netplay/core/text_utils.cpp
    src/netplay/core/validation.cpp
    src/netplay/hooks/menu_hooks.cpp
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
)

target_include_directories(efz_netplay_mod PRIVATE include)
target_compile_features(efz_netplay_mod PRIVATE cxx_std_17)
target_compile_definitions(efz_netplay_mod PRIVATE NOMINMAX)

if(EFZ_EMBEDDED_TLS)
    target_compile_definitions(efz_netplay_mod PRIVATE EFZ_EMBEDDED_TLS=1)
    target_link_libraries(efz_netplay_mod PRIVATE mbedtls mbedx509 mbedcrypto)
endif()

if(MSVC)
    target_compile_options(efz_netplay_mod PRIVATE /W4 /permissive- /EHsc)
    if(EFZ_STATIC_RUNTIME)
        set_property(TARGET efz_netplay_mod PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    if(EFZ_XP_COMPAT)
        target_compile_definitions(efz_netplay_mod PRIVATE WINVER=0x0501 _WIN32_WINNT=0x0501)
        target_compile_options(efz_netplay_mod PRIVATE /Zc:threadSafeInit-)
        if(NOT CMAKE_GENERATOR_TOOLSET MATCHES "v141_xp")
            message(WARNING "EFZ_XP_COMPAT is enabled but generator toolset is not v141_xp; the binary may still import Vista+ APIs.")
        endif()
    endif()
endif()

set_target_properties(efz_netplay_mod PROPERTIES
    OUTPUT_NAME "efz_netplay_mod"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
