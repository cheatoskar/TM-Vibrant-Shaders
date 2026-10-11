cmake_minimum_required(VERSION 3.21)

# TM Vibrant Shaders for ManiaPlanet 4 (TrackMania 2): a d3d11.dll proxy next to ManiaPlanet.exe.
# Separate project from the TMNF plugin (that one is 32-bit D3D9):
#   cmake -S maniaplanet -B build-mp -A x64
#   cmake --build build-mp --config Release
project(tmvs_maniaplanet VERSION 0.1.0 LANGUAGES CXX RC ASM_MASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build the ManiaPlanet proxy for ManiaPlanet.exe (64-bit): configure with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(TMVS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

include(FetchContent)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.5
)
FetchContent_MakeAvailable(imgui)

# Effect shaders, compiled at build time (shader model 4 so feature level 10 cards work too).
set(MP_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mp.hlsl")
set(MP_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${MP_SHADER_OUT}")
file(GLOB MP_SDK_BINS LIST_DIRECTORIES true "C:/Program Files (x86)/Windows Kits/10/bin/10.*")
list(SORT MP_SDK_BINS ORDER DESCENDING)
list(TRANSFORM MP_SDK_BINS APPEND "/x64")
find_program(MP_FXC fxc HINTS "$ENV{WindowsSdkVerBinPath}x64" ${MP_SDK_BINS} REQUIRED)
set(MP_ENTRIES VS_Full:vs_4_0 PS_Down4:ps_4_0 PS_BlurH:ps_4_0 PS_BlurV:ps_4_0 PS_Grade:ps_4_0)
set(MP_SHADER_HEADERS)
foreach(item ${MP_ENTRIES})
  string(REPLACE ":" ";" parts "${item}")
  list(GET parts 0 entry)
  list(GET parts 1 profile)
  set(out "${MP_SHADER_OUT}/${entry}.h")
  add_custom_command(
    OUTPUT "${out}"
    COMMAND "${MP_FXC}" /nologo /O3 /T ${profile} /E ${entry} /Vn g_${entry} /Fh "${out}" "${MP_SHADER}"
    DEPENDS "${MP_SHADER}"
    COMMENT "fxc ${entry}"
    VERBATIM)
  list(APPEND MP_SHADER_HEADERS "${out}")
endforeach()

# Everything but the proxy: shared by the d3d11.dll and the offline previewer.
add_library(tmvs_mp_core STATIC
  src/hooks.cpp
  src/engine_frame.cpp
  src/shaderdb.cpp
  src/renderer.cpp
  src/overlay.cpp
  src/settings.cpp
  src/capture.cpp
  src/log.cpp
  ${TMVS_ROOT}/src/lut.cpp
  ${MP_SHADER_HEADERS}
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(tmvs_mp_core PUBLIC src "${TMVS_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}" ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(tmvs_mp_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
  TMVS_MP_VERSION="${PROJECT_VERSION}")
target_compile_options(tmvs_mp_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /EHsc /permissive- /utf-8>)
# Never d3d11.lib here: the DLL *is* d3d11.dll and would import itself.
target_link_libraries(tmvs_mp_core PUBLIC dxgi dxguid shell32 user32)
set_source_files_properties(
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  PROPERTIES COMPILE_OPTIONS "/W0")

add_library(tmvs_mp SHARED
  src/dllmain.cpp
  src/proxy.cpp
  src/forward.asm
  src/d3d11.def
)
# The game loads it as d3d11.dll from its own folder.
set_target_properties(tmvs_mp PROPERTIES OUTPUT_NAME d3d11)
target_compile_options(tmvs_mp PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /EHsc /permissive- /utf-8>)
target_link_libraries(tmvs_mp PRIVATE tmvs_mp_core)

# Offline previewer: renders a capture (F12 in the game) through the same renderer.
#   mp_preview <capture folder> <out.bmp> [--preset Name] [--view N] [--set Key=Value ...]
add_executable(mp_preview tools/preview.cpp)
target_link_libraries(mp_preview PRIVATE tmvs_mp_core d3d11)
target_compile_options(mp_preview PRIVATE /W4 /EHsc /permissive- /utf-8)

# Offline test: a small D3D11 program that draws like the game (scene with depth, the engine's own
# tone-map pixel shader, then an interface draw) through the proxy and checks where the effects land.
add_executable(mp_harness tools/harness.cpp)
target_link_libraries(mp_harness PRIVATE dxgi dxguid d3dcompiler user32)
target_compile_definitions(mp_harness PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(mp_harness PRIVATE /W4 /EHsc /permissive- /utf-8)
