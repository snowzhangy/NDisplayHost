# The "diag" role of ndisplay.exe (formerly wvd-diag.exe).
add_library(wvd_diag_role STATIC main.cpp firewall.cpp)
target_link_libraries(wvd_diag_role PUBLIC wvd_capture_core wvd_common wvd_quic
  wvd_owner_role)
