add_executable(wvd-network main.cpp)
set_target_properties(wvd-network PROPERTIES CXX_STANDARD 20)
target_link_libraries(wvd-network PRIVATE wvd_common windowsapp wlanapi)
wvd_apply_manifest(wvd-network)
