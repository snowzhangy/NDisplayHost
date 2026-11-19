# The UDP transport (QUIC) and the WIRE.md v1 session on top of it.
#
# Needs the vendored msquic build; without it the whole host still builds and
# runs over TCP, only --quic is unavailable (see WVD_HAVE_MSQUIC).
if(NOT WVD_HAVE_MSQUIC)
  return()
endif()

add_library(wvd_quic STATIC
  quic_host.cpp
  wire1_channel.cpp
  identity.cpp
  wdauth.cpp
)
target_include_directories(wvd_quic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wvd_quic PUBLIC wvd_common wvd_msquic ole32 shell32)
# Consumers test WVD_HAVE_MSQUIC; carrying it on the link line means a tree
# without msquic can never reference code that is not being built.
target_compile_definitions(wvd_quic PUBLIC WVD_HAVE_MSQUIC=1)
