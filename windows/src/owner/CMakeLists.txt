# The "owner" role of ndisplay.exe (formerly wvd-owner.exe): software device,
# CCD topology and keep-awake. Installs and runs as a LocalSystem service while
# remaining the same binary the console uses.
add_library(wvd_owner_role STATIC service_main.cpp device.cpp topology.cpp)
target_include_directories(wvd_owner_role PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wvd_owner_role PUBLIC wvd_common)
