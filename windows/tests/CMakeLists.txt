add_executable(wvd-regressions core_tests.cpp ../src/input/input_state.cpp ../driver/Edid.cpp)
target_include_directories(wvd-regressions PRIVATE ../src/input ../driver)
target_link_libraries(wvd-regressions PRIVATE wvd_capture_core)
add_test(NAME core-regressions COMMAND wvd-regressions)
add_executable(wvd-gpu-tests gpu_tests.cpp)
target_link_libraries(wvd-gpu-tests PRIVATE wvd_capture_core)
add_test(NAME gpu-color-conversion COMMAND wvd-gpu-tests)
add_executable(wvd-delivery-tests delivery_tests.cpp)
target_link_libraries(wvd-delivery-tests PRIVATE wvd_capture_core)
add_test(NAME encoded-reference-delivery COMMAND wvd-delivery-tests)

add_executable(wvd-topology-tests topology_tests.cpp)
target_link_libraries(wvd-topology-tests PRIVATE wvd_common)
add_test(NAME topology-regressions COMMAND wvd-topology-tests)

if(WVD_HAVE_MSQUIC)
  # The QUIC host, loopback: pair, bind the datagram flow, receive an
  # FEC-protected picture, NACK it, drive input. Needs UDP on localhost.
  add_executable(wvd-quic-session-tests quic_session_tests.cpp)
  target_link_libraries(wvd-quic-session-tests PRIVATE wvd_quic)
  wvd_link_msquic(wvd-quic-session-tests)  # msquic.dll beside the exe
  add_test(NAME quic-host-session COMMAND wvd-quic-session-tests)
  set_tests_properties(quic-host-session PROPERTIES TIMEOUT 120)
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_test(NAME input-ipc COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/input_ipc_test.py"
    "$<TARGET_FILE:wvd-input>")
  set_tests_properties(input-ipc PROPERTIES TIMEOUT 20)
  add_test(NAME launcher-stop-and-network-validation COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/launcher_test.py" "$<TARGET_FILE:wvd-media>"
    "$<TARGET_FILE:wvd-input>" "$<TARGET_FILE:wvd-network>")
  set_tests_properties(launcher-stop-and-network-validation PROPERTIES TIMEOUT 15)
endif()
