add_executable(wvd-input
  main.cpp
  input_state.cpp
)
target_include_directories(wvd-input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wvd-input PRIVATE wvd_common)
set_target_properties(wvd-input PROPERTIES OUTPUT_NAME "wvd-input")
wvd_apply_manifest(wvd-input)
