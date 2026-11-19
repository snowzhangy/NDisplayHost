add_executable(wvd-testcard main.cpp)
target_link_libraries(wvd-testcard PRIVATE wvd_common)
set_target_properties(wvd-testcard PROPERTIES OUTPUT_NAME "wvd-testcard")
wvd_apply_manifest(wvd-testcard)
