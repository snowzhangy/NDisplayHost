add_executable(wvd-owner
  service_main.cpp
  device.cpp
  topology.cpp
)
target_include_directories(wvd-owner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wvd-owner PRIVATE wvd_common)
set_target_properties(wvd-owner PROPERTIES OUTPUT_NAME "wvd-owner")
wvd_apply_manifest(wvd-owner)
