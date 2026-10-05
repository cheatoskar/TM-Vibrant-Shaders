cmake_minimum_required(VERSION 3.21)

project(tm_vibrant_shaders VERSION 1.0.0 LANGUAGES CXX)

if(NOT DEFINED TM_SHADERS_VERSION)
  set(TM_SHADERS_VERSION "${PROJECT_VERSION}")
endif()
message(STATUS "TM Vibrant Shaders version: ${TM_SHADERS_VERSION}")

# TrackMania Forever is 32-bit (DirectX 9)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "TrackMania Forever is 32-bit: please configure with -A Win32.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
cmake_policy(SET CMP0091 NEW)

# Statically linked runtime (/MT) so no extra MSVC redistributable is required
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.5
)
FetchContent_MakeAvailable(imgui)

configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/tm_shaders_version.h" @ONLY)

add_library(tm_vibrant_shaders SHARED
  src/dllmain.cpp
  src/config.cpp
  src/hook.cpp
  src/overlay.cpp

  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)

target_include_directories(tm_vibrant_shaders PRIVATE
  src
  ${CMAKE_CURRENT_BINARY_DIR}
  ${imgui_SOURCE_DIR}
)

target_compile_definitions(tm_vibrant_shaders PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
)

target_compile_options(tm_vibrant_shaders PRIVATE /W4 /EHsc /permissive-)

set_target_properties(tm_vibrant_shaders PROPERTIES
  OUTPUT_NAME "TMVibrantShaders"
  PREFIX ""
)

add_custom_command(TARGET tm_vibrant_shaders POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_FILE:tm_vibrant_shaders>
          $<TARGET_FILE_DIR:tm_vibrant_shaders>/TMVibrantShaders.asi
  COMMENT "Copying TMVibrantShaders.dll to TMVibrantShaders.asi"
)
