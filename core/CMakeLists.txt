cmake_minimum_required(VERSION 3.24)
project(NDisplayHostCore LANGUAGES CXX)

add_library(wvd_wire_core STATIC
  src/fec.cpp
  src/wire1.cpp
  src/wire1_msg.cpp
)
target_include_directories(wvd_wire_core PUBLIC include)
target_compile_features(wvd_wire_core PUBLIC cxx_std_17)

if(PROJECT_IS_TOP_LEVEL)
  include(CTest)
endif()
if(BUILD_TESTING)
  add_executable(wvd-wire1-tests tests/wire1_tests.cpp)
  target_link_libraries(wvd-wire1-tests PRIVATE wvd_wire_core)
  add_test(NAME wire1-codec-and-fec COMMAND wvd-wire1-tests)
endif()
