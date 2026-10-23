# Keep this mpv build paired with AJI v0.11.1 (ABI v9) in Dart's installer.
# Compile and bundle against the same verified libmpv archive.
if(NOT FLUTTER_TARGET_PLATFORM STREQUAL "windows-x64")
  message(FATAL_ERROR "AnimeJaNai requires Windows x64")
endif()
set(AJN_DIR "${CMAKE_BINARY_DIR}/animejanai")
file(MAKE_DIRECTORY "${AJN_DIR}")
function(ajn_archive name url digest)
  set(archive "${AJN_DIR}/${name}")
  if(EXISTS "${archive}")
    file(SHA256 "${archive}" actual)
    if(NOT actual STREQUAL digest)
      file(REMOVE "${archive}")
    endif()
  endif()
  if(NOT EXISTS "${archive}")
    file(DOWNLOAD "${url}" "${archive}" EXPECTED_HASH "SHA256=${digest}"
      TLS_VERIFY ON TIMEOUT 180 STATUS download_status)
    list(GET download_status 0 download_code)
    if(NOT download_code EQUAL 0)
      message(FATAL_ERROR "AnimeJaNai download failed: ${download_status}")
    endif()
  endif()
  set(extracted "${AJN_DIR}/${name}-extracted")
  set(extracted_digest "")
  if(EXISTS "${extracted}/.complete")
    file(READ "${extracted}/.complete" extracted_digest)
  endif()
  if(NOT extracted_digest STREQUAL digest)
    file(MAKE_DIRECTORY "${extracted}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${archive}"
      WORKING_DIRECTORY "${extracted}" RESULT_VARIABLE extract_result)
    if(NOT extract_result EQUAL 0)
      message(FATAL_ERROR "Could not extract AnimeJaNai ${name}")
    endif()
    file(WRITE "${extracted}/.complete" "${digest}")
  endif()
endfunction()

ajn_archive(mpv-dev.7z
  "https://github.com/the-database/mpv-winbuild/releases/download/2026-10-09-19c0fcf895/mpv-dev-x86_64-20261009-git-19c0fcf895.7z"
  8801c722b5174a751d03f1cb435092c61304cd8605302fe7cae2cc77872b2d83)
ajn_archive(7z2602-extra.7z "https://www.7-zip.org/a/7z2602-extra.7z"
  081df9e9311dfd9c9e0e98c1c80180b99bb51e4cb24156b5f3057fe3c259d70a)

# Export one runtime, import library and header set to the video plugin.
set(LIBMPV_SRC "${AJN_DIR}/mpv-dev.7z-extracted")
set(LIBMPV_INCLUDE_DIR "${LIBMPV_SRC}/include/mpv")
install(FILES "${AJN_DIR}/7z2602-extra.7z-extracted/x64/7za.exe"
  DESTINATION "$<TARGET_FILE_DIR:${BINARY_NAME}>/data/animejanai" COMPONENT Runtime)
install(FILES "${AJN_DIR}/7z2602-extra.7z-extracted/License.txt"
  "${CMAKE_CURRENT_SOURCE_DIR}/animejanai-NOTICE.txt"
  DESTINATION "$<TARGET_FILE_DIR:${BINARY_NAME}>/data/licenses/animejanai" COMPONENT Runtime)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/AniBaka_GPLv3.txt"
  DESTINATION "$<TARGET_FILE_DIR:${BINARY_NAME}>/data/licenses/animejanai"
  RENAME AniBaka_GPLv3.txt COMPONENT Runtime)
