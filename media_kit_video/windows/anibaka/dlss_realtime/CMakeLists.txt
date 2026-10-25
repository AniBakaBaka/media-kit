cmake_minimum_required(VERSION 3.15)
project(baka_dlss_gpu LANGUAGES CXX)
include(ngx_sdk.cmake)
add_library(baka_dlss_gpu STATIC gpu.cpp ngx_features.cpp video_bridge.cpp comparison_labels.cpp vendor/archspoof.cpp)
set_property(TARGET baka_dlss_gpu PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_compile_features(baka_dlss_gpu PUBLIC cxx_std_17)
target_compile_options(baka_dlss_gpu PRIVATE /W4 /WX /EHsc /utf-8)
target_compile_definitions(baka_dlss_gpu PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(baka_dlss_gpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(baka_dlss_gpu SYSTEM PRIVATE "${NGX_SDK_DIR}")
target_link_libraries(baka_dlss_gpu PRIVATE d3d12 d3d11 dxgi d3dcompiler dxguid gdi32)
target_link_libraries(baka_dlss_gpu PRIVATE
  "$<$<CONFIG:Debug>:${NGX_SDK_DIR}/nvsdk_ngx_d_dbg.lib>"
  "$<$<NOT:$<CONFIG:Debug>>:${NGX_SDK_DIR}/nvsdk_ngx_d.lib>")

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/LICENSE"
  DESTINATION "$<TARGET_FILE_DIR:${BINARY_NAME}>/data/licenses/video2dlssnr"
  COMPONENT Runtime)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
  DESTINATION "$<TARGET_FILE_DIR:${BINARY_NAME}>/data/licenses/media_kit_video_windows"
  RENAME AniBaka_GPLv3.txt COMPONENT Runtime)