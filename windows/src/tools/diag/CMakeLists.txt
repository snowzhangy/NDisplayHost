add_executable(wvd-diag
  main.cpp
  ../../owner/topology.cpp
)
target_link_libraries(wvd-diag PRIVATE wvd_capture_core wvd_common wvd_quic)
wvd_link_msquic(wvd-diag)
set_target_properties(wvd-diag PROPERTIES OUTPUT_NAME "wvd-diag")
wvd_apply_manifest(wvd-diag)
