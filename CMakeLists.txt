cmake_minimum_required(VERSION 3.21)

# Static CRT must be decided before any target exists.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:DEBUG>:Debug>DLL")

project(
    CustomMusicSystem
    VERSION 2.6.6
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# /utf-8: sources carry UTF-8 Chinese comments.
# /Zc:__cplusplus: CommonLibSSE headers gate on __cplusplus.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8 /EHsc /permissive- /Zc:__cplusplus /W3 /MP")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi /FS /GL")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG:FULL /LTCG /OPT:REF /OPT:ICF")

# header-only spdlog / fmt / detours assembled under D:/dev/deps (no vcpkg on this machine)
list(APPEND CMAKE_PREFIX_PATH "D:/dev/deps")

set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)

# This CommonLibSSE-NG fork only defines ENABLE_SKYRIM_AE while a few headers
# gate the AE member layout behind SKYRIM_SUPPORT_AE.  Must be set before
# add_subdirectory so the static library uses the same layout as the plugin.
add_compile_definitions(SKYRIM_SUPPORT_AE=1)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)

add_subdirectory("D:/dev/CommonLibSSE-NG" "${CMAKE_CURRENT_BINARY_DIR}/CommonLibSSE-NG")

# ------------------------------------------------------------- two builds ---
# Same source, two DLLs: the UI language is a compile-time constant (see
# src/i18n.h).  That removes the loose translation files, the sLanguage probe
# and the runtime switch in one go - the three things that made "switch to
# Chinese" unreliable.
set(CMS_CORE_SRC
    src/core.cpp
    src/audio.cpp
    src/director.cpp
    src/volume.cpp
    src/i18n.cpp
    src/menu_page.cpp
)
set(CMS_PLUGIN_SRC
    src/main.cpp
    src/platform.cpp
    src/menu.cpp
    src/fwio.cpp
)

function(cms_add_build suffix zh output_name)
    add_library(CustomMusicSystemCore${suffix} STATIC ${CMS_CORE_SRC})
    target_include_directories(CustomMusicSystemCore${suffix} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(CustomMusicSystemCore${suffix} PUBLIC cxx_std_23)
    target_compile_definitions(CustomMusicSystemCore${suffix} PUBLIC CMS_UI_LANG_ZH=${zh})
    target_link_libraries(CustomMusicSystemCore${suffix} PUBLIC xaudio2 ole32)

    add_library(CustomMusicSystemPlugin${suffix} SHARED ${CMS_PLUGIN_SRC})
    target_include_directories(CustomMusicSystemPlugin${suffix} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/extern")
    target_link_libraries(CustomMusicSystemPlugin${suffix} PRIVATE CustomMusicSystemCore${suffix} CommonLibSSE::CommonLibSSE)
    set_target_properties(CustomMusicSystemPlugin${suffix} PROPERTIES
        OUTPUT_NAME "${output_name}"
        PREFIX ""
        PDB_NAME "${output_name}"
        COMPILE_PDB_NAME "${output_name}_compile"
    )
endfunction()

# Chinese build -> CustomMusicSystem.dll          (the one to ship by default here)
cms_add_build(""    1 "CustomMusicSystem")
# English build -> CustomMusicSystem_en.dll       (rename to CustomMusicSystem.dll to use)
cms_add_build("_en" 0 "CustomMusicSystem_en")

# ---------------------------------------------------------------------- tests
add_executable(CustomMusicSystemTests tests/tests.cpp)
target_link_libraries(CustomMusicSystemTests PRIVATE CustomMusicSystemCore)

# The same suite against the English build: the language is a compile-time
# constant, so only a binary built with CMS_UI_LANG_ZH=0 can prove that the
# English table is the one that reached the DLL.
add_executable(CustomMusicSystemTests_en tests/tests.cpp)
target_link_libraries(CustomMusicSystemTests_en PRIVATE CustomMusicSystemCore_en)

add_executable(CustomMusicSystemPlayTest tests/play_test.cpp)
target_link_libraries(CustomMusicSystemPlayTest PRIVATE CustomMusicSystemCore)
