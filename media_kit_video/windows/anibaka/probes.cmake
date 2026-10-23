# Developer-only integration target; never part of the application bundle.
add_executable(baka_video_pipeline_probe EXCLUDE_FROM_ALL
  "${CMAKE_CURRENT_SOURCE_DIR}/../tool/video_pipeline_probe/main.cc"
  "${VIDEO_SOURCE_DIR}/native_gl_surface.cc"
  "${VIDEO_SOURCE_DIR}/angle_surface_manager.cc"
  "${VIDEO_SOURCE_DIR}/video_state.cc"
  "${VIDEO_SOURCE_DIR}/utils.cc")
target_compile_features(baka_video_pipeline_probe PRIVATE cxx_std_17)
target_compile_definitions(baka_video_pipeline_probe PRIVATE NOMINMAX)
target_compile_options(baka_video_pipeline_probe PRIVATE /utf-8 /W4 /WX /wd4100)
target_include_directories(baka_video_pipeline_probe PRIVATE
  "${VIDEO_SOURCE_DIR}" "${LIBMPV_INCLUDE_DIR}" "${ANGLE_SRC}/include")
target_link_libraries(baka_video_pipeline_probe PRIVATE
  "${LIBMPV_SRC}/libmpv.dll.a"
  "${ANGLE_SRC}/lib/libEGL.dll.lib"
  "${ANGLE_SRC}/lib/libGLESv2.dll.lib"
  opengl32 d3d11 dxgi gdi32 user32 winmm)
set_target_properties(baka_video_pipeline_probe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/video-probe")