# Capture/encode core shared by wvd-media and wvd-diag.
add_library(wvd_capture_core STATIC
  dx.cpp
  display_modes.cpp
  capture/dda_capture.cpp
  preprocess/nv12_convert.cpp
  encode/factory.cpp
  encode/null_encoder.cpp
  encode/d3d12ve_probe.cpp
  encode/nvenc_encoder.cpp
  encode/mf_encoder.cpp
  encode/vendor_stubs.cpp
  cursor/cursor_source.cpp
  audio/loopback_capture.cpp
  screen_curtain.cpp
  session/media_session.cpp
)
target_include_directories(wvd_capture_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wvd_capture_core PUBLIC wvd_common d3d11 dxgi d3dcompiler dnsapi
  mfplat mfuuid ole32 oleaut32 uuid user32)

if(DEFINED NVENC_INCLUDE_DIR)
  # NVENC entry points are resolved at runtime from nvEncodeAPI64.dll (§4.6).
  target_include_directories(wvd_capture_core PUBLIC ${NVENC_INCLUDE_DIR})
  target_compile_definitions(wvd_capture_core PRIVATE WVD_HAVE_NVENC=1)
endif()

add_executable(wvd-media main.cpp discovery.cpp)
target_link_libraries(wvd-media PRIVATE wvd_capture_core wvd_quic)
wvd_link_msquic(wvd-media)
set_target_properties(wvd-media PROPERTIES OUTPUT_NAME "wvd-media")
wvd_apply_manifest(wvd-media)
