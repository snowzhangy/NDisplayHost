# The "input" role of ndisplay.exe (formerly wvd-input.exe). The role stays a
# separate process: it runs in the interactive session and is the only piece
# that may be elevated, so arming it is a decision the operator can see.
add_library(wvd_input_role STATIC main.cpp input_state.cpp)
target_include_directories(wvd_input_role PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wvd_input_role PUBLIC wvd_common)
