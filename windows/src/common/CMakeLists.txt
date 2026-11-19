add_library(wvd_common STATIC
  logging.cpp
  file.cpp
  ipc.cpp
  power.cpp
)
target_include_directories(wvd_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wvd_common PUBLIC
  ws2_32 advapi32 wtsapi32 shlwapi shell32 ole32)
