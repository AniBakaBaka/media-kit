# This file is a part of media_kit (https://github.com/media-kit/media-kit).
#
# Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
# All rights reserved.
# Use of this source code is governed by MIT license that can be found in the LICENSE file.

cmake_minimum_required(VERSION 3.14)

set(PROJECT_NAME "media_kit_video")
set(CMAKE_CXX_STANDARD 17)
project(${PROJECT_NAME} LANGUAGES CXX)

set(PLUGIN_NAME "media_kit_video_plugin")

option(MEDIA_KIT_VIDEO_ENABLE_DLSS "Enable AniBaka's Windows video bridge" OFF)
set(VIDEO_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(MEDIA_KIT_VIDEO_ENABLE_DLSS)
  if(NOT TARGET baka_dlss_gpu)
    add_subdirectory(anibaka/dlss_realtime)
  endif()
  set(VIDEO_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/anibaka")
endif()

# This is shipped as part of package:media_kit_libs_*** package(s).
# Must be built before this target.
set(LIBMPV_SRC "${CMAKE_BINARY_DIR}/libmpv")
set(LIBMPV_INCLUDE_DIR "${LIBMPV_SRC}/include")
if(DEFINED MEDIA_KIT_LIBMPV_ROOT)
  set(LIBMPV_SRC "${MEDIA_KIT_LIBMPV_ROOT}")
  set(LIBMPV_INCLUDE_DIR "${MEDIA_KIT_LIBMPV_INCLUDE_DIR}")
endif()
set(ANGLE_SRC "${CMAKE_BINARY_DIR}/ANGLE")

# Deal with MSVC incompatiblity
add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)

if(MEDIA_KIT_LIBS_AVAILABLE)
  include_directories(
    "${LIBMPV_INCLUDE_DIR}"
    "${ANGLE_SRC}/include"
  )

  add_library(
    ${PLUGIN_NAME} SHARED
    "${VIDEO_SOURCE_DIR}/angle_surface_manager.cc"
    "${VIDEO_SOURCE_DIR}/media_kit_video_plugin_c_api.cc"
    "${VIDEO_SOURCE_DIR}/media_kit_video_plugin.cc"
    "${VIDEO_SOURCE_DIR}/video_output_manager.cc"
    "${VIDEO_SOURCE_DIR}/video_output.cc"
    "${VIDEO_SOURCE_DIR}/utils.cc"
    ${PLUGIN_SOURCES}
  )

  apply_standard_settings(${PLUGIN_NAME})

  if(MEDIA_KIT_VIDEO_ENABLE_DLSS)
    target_sources(${PLUGIN_NAME} PRIVATE
      "${VIDEO_SOURCE_DIR}/video_state.cc"
      "${VIDEO_SOURCE_DIR}/native_gl_surface.cc")
    target_link_libraries(${PLUGIN_NAME} PRIVATE
      baka_dlss_gpu d3d11 dxgi gdi32 user32)
    include("${VIDEO_SOURCE_DIR}/probes.cmake")
    target_compile_definitions(${PLUGIN_NAME} PRIVATE NOMINMAX)
    target_compile_options(${PLUGIN_NAME} PRIVATE /utf-8)
  endif()

  set_target_properties(
    ${PLUGIN_NAME} PROPERTIES
    CXX_VISIBILITY_PRESET hidden
  )
  target_compile_definitions(
    ${PLUGIN_NAME} PRIVATE
    FLUTTER_PLUGIN_IMPL
  )

  target_include_directories(
    ${PLUGIN_NAME} INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
  )

  target_link_libraries(
    ${PLUGIN_NAME} PRIVATE
    flutter
    flutter_wrapper_plugin

    # Link to libmpv & ANGLE.
    "${LIBMPV_SRC}/libmpv.dll.a"
    "${ANGLE_SRC}/lib/libEGL.dll.lib"
    "${ANGLE_SRC}/lib/libGLESv2.dll.lib"
  )
  if(MEDIA_KIT_VIDEO_ENABLE_DLSS)
    # GLES exports must resolve before opengl32's overlapping GL symbols.
    # The native interop path resolves desktop GL explicitly through WGL.
    target_link_libraries(${PLUGIN_NAME} PRIVATE opengl32)
  endif()
else()
  message(NOTICE "media_kit: WARNING: package:media_kit_libs_*** not found.")

  add_library(
    ${PLUGIN_NAME} SHARED
    "media_kit_video_plugin_c_api.cc"
    ${PLUGIN_SOURCES}
  )

  apply_standard_settings(${PLUGIN_NAME})

  set_target_properties(
    ${PLUGIN_NAME} PROPERTIES
    CXX_VISIBILITY_PRESET hidden
  )

  target_compile_definitions(
    ${PLUGIN_NAME} PRIVATE
    FLUTTER_PLUGIN_IMPL

    # Add macro to indicate that libmpv & ANGLE from package:media_kit_libs_*** are not found.
    "MEDIA_KIT_LIBS_NOT_FOUND=1"
  )

  target_include_directories(
    ${PLUGIN_NAME} INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
  )

  target_link_libraries(
    ${PLUGIN_NAME} PRIVATE
    flutter
    flutter_wrapper_plugin
  )
endif()

set(
  media_kit_video_bundled_libraries
  ""
  PARENT_SCOPE
)
