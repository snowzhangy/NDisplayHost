# The command-line host: one executable, one role per process.
#   ndisplay stream|input|owner|network|diag| --version
add_executable(ndisplay main.cpp)
target_link_libraries(ndisplay PRIVATE
  wvd_stream_role wvd_input_role wvd_owner_role wvd_network_role wvd_diag_role
  wvd_common)
wvd_link_msquic(ndisplay)
# Both shipped executables land in build/bin, so ndisplay.exe sits next to
# NDisplayHostWin.exe and msquic.dll, exactly as in the release package.
set_target_properties(ndisplay PROPERTIES OUTPUT_NAME "ndisplay"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(ndisplay PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config}
    "${CMAKE_BINARY_DIR}/bin")
endforeach()
wvd_apply_manifest(ndisplay)
