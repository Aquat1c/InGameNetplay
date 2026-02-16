cmake_minimum_required(VERSION 3.21)
project(EFZInGameNetplayMod VERSION 0.1.0 LANGUAGES CXX)

add_library(efz_netplay_mod SHARED
    src/dllmain.cpp
    src/logger.cpp
    src/netplay_menu_hooks.cpp
)

target_include_directories(efz_netplay_mod PRIVATE include)
target_compile_features(efz_netplay_mod PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(efz_netplay_mod PRIVATE /W4 /permissive- /EHsc)
endif()

set_target_properties(efz_netplay_mod PROPERTIES
    OUTPUT_NAME "efz_netplay_mod"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

