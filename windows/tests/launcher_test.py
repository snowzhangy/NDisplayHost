"""Validate launcher stop events and network arguments without activating Wi-Fi."""
import ctypes
import os
import socket
import tempfile
import time
import subprocess
import sys
import uuid

kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.CreateEventW.argtypes = [ctypes.c_void_p, ctypes.c_bool, ctypes.c_bool, ctypes.c_wchar_p]
kernel.CreateEventW.restype = ctypes.c_void_p
kernel.CloseHandle.argtypes = [ctypes.c_void_p]
name = "Local\\WvdTestStop-" + uuid.uuid4().hex
event = kernel.CreateEventW(None, True, True, name)
assert event

# One executable for every host role; each call names the role it starts.
CLI = sys.argv[1]

def run(role, arguments, text=""):
    return subprocess.run([CLI, role] + arguments, input=text, text=True,
                          capture_output=True, timeout=4, creationflags=subprocess.CREATE_NO_WINDOW)

try:
    for role, extra, stdin in [("stream", [], ""), ("input", [], ""),
                               ("network", ["--ssid", "WVD-Stop-Test"], "TestPassword123\n")]:
        result = run(role, extra + ["--stop-event", name], stdin)
        assert result.returncode == 0, result.stderr
        assert "DIRECT_WIFI_STARTED" not in result.stdout
    for ssid, password in [("", "TestPassword123"), ("X" * 33, "TestPassword123"),
                           ("WVD-Test", "short"), ("WVD-Test", "x" * 64)]:
        result = run("network", ["--ssid", ssid], password + "\n")
        assert result.returncode == 2, (ssid, result.returncode, result.stderr)
        assert "DIRECT_WIFI_STARTED" not in result.stdout
    for value in ("0", "65536", "abc", "47800garbage"):
        assert run("stream", ["--listen", value]).returncode == 2, value
    kernel.ResetEvent.argtypes = [ctypes.c_void_p]
    kernel.SetEvent.argtypes = [ctypes.c_void_p]
    assert kernel.ResetEvent(event)
    # Hold the same number in the TCP namespace throughout startup.
    # A TCP listener can coexist while the host answers a real UDP handshake.
    with tempfile.TemporaryDirectory(prefix="wvm-udp-test-") as home, socket.socket() as tcp:
        tcp.bind(("127.0.0.1", 0))
        tcp.listen()
        port = tcp.getsockname()[1]
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            udp.bind(("0.0.0.0", port))
            blocked = subprocess.run([CLI, "stream", "--listen", str(port), "--no-discovery",
                                      "--no-pair"], capture_output=True, text=True, timeout=4,
                                     env={**os.environ, "WVM_HOME": home},
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            assert blocked.returncode == 1, (blocked.stdout, blocked.stderr)
            assert f"HOST_LISTEN_ERROR UDP {port}:" in blocked.stdout, blocked.stdout
        with tempfile.TemporaryFile(mode="w+t", encoding="utf-8") as logs:
            host = subprocess.Popen([CLI, "stream", "--listen", str(port), "--no-discovery", "--no-pair",
                                       "--stop-event", name],
                                      stdout=logs, stderr=subprocess.STDOUT, text=True,
                                      env={**os.environ, "WVM_HOME": home},
                                      creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and host.poll() is None:
                    logs.seek(0)
                    if f"HOST_READY {port}" in logs.read(): break
                    time.sleep(0.05)
                else:
                    logs.seek(0)
                    raise AssertionError(logs.read())
                probe = run("diag", ["quic-probe", str(port)])
                assert probe.returncode == 0, (probe.stdout, probe.stderr)
                assert "datagram handshake succeeded" in probe.stdout
                assert kernel.SetEvent(event)
                assert host.wait(5) == 0
            finally:
                if host.poll() is None:
                    host.kill()
                    host.wait(5)
    print("launcher stop events and UDP-only listener and offline-network argument validation passed")
finally:
    kernel.CloseHandle(event)
