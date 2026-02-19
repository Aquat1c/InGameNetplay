cmake_minimum_required(VERSION 3.21)
project(EFZInGameNetplayMod VERSION 0.1.0 LANGUAGES CXX)

add_library(efz_netplay_mod SHARED
    src/dllmain.cpp
    src/logger.cpp
    src/netplay/assets/assets.cpp
    src/netplay/core/inline_edit.cpp
    src/netplay/core/input_utils.cpp
    src/netplay/core/lobby_client.cpp
    src/netplay/core/menu_model.cpp
    src/netplay/core/patch_utils.cpp
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
target_link_libraries(efz_netplay_mod PRIVATE winhttp)

if(MSVC)
    target_compile_options(efz_netplay_mod PRIVATE /W4 /permissive- /EHsc)
endif()

set_target_properties(efz_netplay_mod PROPERTIES
    OUTPUT_NAME "efz_netplay_mod"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
