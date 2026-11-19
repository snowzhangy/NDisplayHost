# NDisplayHostWin — NDisplay Windows host for Apple Vision Pro (plan v2.3)
#
# User-mode components build with plain MSVC + Windows SDK 10.0.22621+.
# The IddCx UMDF driver (driver/) additionally requires the WDK and is NOT
# part of this build by default (see driver/README.md).
cmake_minimum_required(VERSION 3.24)
project(WVD LANGUAGES CXX)

# Product identity shown to the user: "NDisplayHostWin <version> (build N)".
# THIS IS THE ONLY PLACE THE VERSION LIVES — bump it on every build you hand
# out. It reaches the control-panel title/tray tooltip through
# src/ui/Version.cs.in and the worker logs through wvd/version.h.
set(WVM_HOST_NAME "NDisplayHostWin")
set(WVM_HOST_VERSION_MAJOR 1)
set(WVM_HOST_VERSION_MINOR 0)
set(WVM_HOST_VERSION_PATCH 0)
set(WVM_HOST_BUILD 1)
string(JOIN "." WVM_HOST_VERSION "${WVM_HOST_VERSION_MAJOR}" "${WVM_HOST_VERSION_MINOR}"
     "${WVM_HOST_VERSION_PATCH}")
add_compile_definitions(
  WVM_HOST_VERSION_MAJOR=${WVM_HOST_VERSION_MAJOR}
  WVM_HOST_VERSION_MINOR=${WVM_HOST_VERSION_MINOR}
  WVM_HOST_VERSION_PATCH=${WVM_HOST_VERSION_PATCH}
  WVM_HOST_BUILD=${WVM_HOST_BUILD}
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
  message(FATAL_ERROR "WVD builds with MSVC on Windows (see scripts/build.ps1)")
endif()

add_compile_definitions(
  UNICODE _UNICODE
  _CRT_SECURE_NO_WARNINGS
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _WIN32_WINNT=0x0A00
  WINVER=0x0A00
  # Windows 11 22H2 target (§3: "Windows 11 first"). NTDDI must be >= WIN10_RS2
  # for DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 and friends.
  NTDDI_VERSION=0x0A00000B
)
add_compile_options(/W4 /EHsc /permissive- /utf-8 /Zc:__cplusplus)

# Optional drop-in SDKs (§4.6): point WVD_NVENC_SDK_DIR at the NVIDIA Video
# Codec SDK (includes/nvEncodeAPI.h). Without it the NVENC module compiles to
# an unavailable stub; selection falls through the tier chain.
option(WVD_ENABLE_NVENC "Build the NVENC encoder module" ON)
set(WVD_NVENC_SDK_DIR "" CACHE PATH "NVIDIA Video Codec SDK root (contains include/nvEncodeAPI.h)")
if(WVD_ENABLE_NVENC AND WVD_NVENC_SDK_DIR)
  find_path(NVENC_INCLUDE_DIR nvEncodeAPI.h PATHS "${WVD_NVENC_SDK_DIR}/Interface"
    "${WVD_NVENC_SDK_DIR}/include" NO_DEFAULT_PATH NO_CACHE)
  if(NOT EXISTS "${NVENC_INCLUDE_DIR}/nvEncodeAPI.h")
    message(WARNING "WVD_NVENC_SDK_DIR set but nvEncodeAPI.h not found; NVENC stays a stub")
    unset(NVENC_INCLUDE_DIR)
  endif()
endif()

# The Windows host uses MsQuic with Schannel TLS.
set(WVD_MSQUIC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/msquic" CACHE PATH "MsQuic source and build tree")
set(WVD_MSQUIC_LIB "${WVD_MSQUIC_ROOT}/build/obj/Release/msquic.lib")
set(WVD_MSQUIC_DLL "${WVD_MSQUIC_ROOT}/build/bin/Release/msquic.dll")
if(NOT EXISTS "${WVD_MSQUIC_ROOT}/src/inc/msquic.h" OR
   NOT EXISTS "${WVD_MSQUIC_LIB}" OR NOT EXISTS "${WVD_MSQUIC_DLL}")
  message(FATAL_ERROR "MsQuic is required. Run windows/scripts/build-msquic.ps1 from an x64 Visual Studio developer shell.")
endif()
add_library(wvd_msquic INTERFACE)
target_include_directories(wvd_msquic INTERFACE "${WVD_MSQUIC_ROOT}/src/inc")
target_link_libraries(wvd_msquic INTERFACE "${WVD_MSQUIC_LIB}" bcrypt crypt32 ncrypt)
# Link msquic into an executable and put the runtime next to it.
function(wvd_link_msquic target)
  target_link_libraries(${target} PRIVATE wvd_msquic)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WVD_MSQUIC_DLL}"
            $<TARGET_FILE_DIR:${target}>/msquic.dll)
endfunction()

# Every host executable embeds wvd.manifest (per-monitor-v2 DPI awareness).
# Coordinate correctness (§4.1 Path D) depends on it: DPI-virtualised desktop
# coordinates do NOT match the physical pixels Desktop Duplication delivers.
enable_language(RC)
set(WVD_MANIFEST_RC "${CMAKE_CURRENT_SOURCE_DIR}/wvd.manifest.rc")
function(wvd_apply_manifest target)
  if(MSVC)
    # Compiled straight into the exe as RT_MANIFEST id 1; /MANIFEST:NO stops
    # link.exe generating a second one (duplicate-resource CVT1100 otherwise).
    target_sources(${target} PRIVATE ${WVD_MANIFEST_RC})
    target_link_options(${target} PRIVATE "/MANIFEST:NO")
  endif()
endfunction()

include(CTest)
add_subdirectory(../core core)
add_subdirectory(src/common)
add_subdirectory(src/media/quic)
add_subdirectory(src/media)
add_subdirectory(src/owner)
add_subdirectory(src/input)
add_subdirectory(src/tools/diag)
add_subdirectory(src/tools/quicspike)
add_subdirectory(src/tools/testcard)
add_subdirectory(src/network)
add_subdirectory(src/ui)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
