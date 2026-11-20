# The "network" role of ndisplay.exe (formerly wvd-network.exe): an offline
# access point using the public Wi-Fi Direct legacy-AP desktop API.
add_library(wvd_network_role STATIC main.cpp)
set_target_properties(wvd_network_role PROPERTIES CXX_STANDARD 20)
target_link_libraries(wvd_network_role PUBLIC wvd_common windowsapp wlanapi)
