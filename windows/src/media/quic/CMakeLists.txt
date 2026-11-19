# The UDP transport (QUIC) and the WIRE.md v1 session on top of it.
#
add_library(wvd_quic STATIC
  quic_host.cpp
  wire1_channel.cpp
  identity.cpp
  wdauth.cpp
)
target_include_directories(wvd_quic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wvd_quic PUBLIC wvd_common wvd_wire_core wvd_msquic ole32 shell32)
