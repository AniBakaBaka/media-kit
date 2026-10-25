# Fetch the official SDK headers and libraries at a fixed revision. Keep SDK
# sources in the build directory; ship its license with the application.
set(NGX_REVISION "374959484e79a640feaba44c93ac8cfb0a03f5b5")
set(NGX_SDK_DIR "${CMAKE_CURRENT_BINARY_DIR}/ngx-sdk")
file(MAKE_DIRECTORY "${NGX_SDK_DIR}")
function(ngx_fetch remote name digest)
  set(destination "${NGX_SDK_DIR}/${name}")
  if(EXISTS "${destination}")
    file(SHA256 "${destination}" actual)
    if(actual STREQUAL digest)
      return()
    endif()
  endif()
  file(DOWNLOAD
    "https://raw.githubusercontent.com/NVIDIA/DLSS/${NGX_REVISION}/${remote}"
    "${destination}" EXPECTED_HASH "SHA256=${digest}" TLS_VERIFY ON
    TIMEOUT 120 STATUS status)
  list(GET status 0 code)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "Could not fetch NVIDIA NGX SDK: ${status}")
  endif()
endfunction()
ngx_fetch(include/nvsdk_ngx_defs.h nvsdk_ngx_defs.h ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e)
ngx_fetch(include/nvsdk_ngx.h nvsdk_ngx.h dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848)
ngx_fetch(include/nvsdk_ngx_params.h nvsdk_ngx_params.h 943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc)
ngx_fetch(include/nvsdk_ngx_defs_dlssg.h nvsdk_ngx_defs_dlssg.h 98ec3a6faa5aa250828c174bf8f3491aafac693dfe57cc798049c8918597fc60)
ngx_fetch(LICENSE.txt LICENSE.txt d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0)
ngx_fetch(lib/Windows_x86_64/x64/nvsdk_ngx_d.lib nvsdk_ngx_d.lib 4b6cecad7f1906571c94010241f650e4a5457e64fad49ddaccb82de79f6c2999)
ngx_fetch(lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib nvsdk_ngx_d_dbg.lib 712f18162c8c8766c92c98031b52d2ba34ee0ad027ba79731278ee6b63cd3e02)
# Flutter stores a generator expression in CMAKE_INSTALL_PREFIX. A relative
# destination appends that expression at install time without expanding it.
# Put the expression directly in DESTINATION so CMake resolves each build mode.
if(DEFINED BINARY_NAME AND TARGET "${BINARY_NAME}")
  set(NGX_LICENSE_DESTINATION "$<TARGET_FILE_DIR:${BINARY_NAME}>/data/licenses/nvidia-dlss")
else()
  set(NGX_LICENSE_DESTINATION "data/licenses/nvidia-dlss")
endif()
install(FILES "${NGX_SDK_DIR}/LICENSE.txt"
  DESTINATION "${NGX_LICENSE_DESTINATION}" COMPONENT Runtime)
