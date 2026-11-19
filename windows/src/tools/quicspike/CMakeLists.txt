# QUIC spike — validates the vendored msquic stack end to end on this machine.
# Not shipped; it checks the things WIRE.md depends on (handshake, TLS exporter
# secrets, datagrams) before any headset is involved.
add_executable(wvd-quic-spike main.cpp)
target_link_libraries(wvd-quic-spike PRIVATE wvd_quic)
