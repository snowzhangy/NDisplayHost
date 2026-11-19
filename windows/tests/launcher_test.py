"""Validate launcher stop events and network arguments without activating Wi-Fi."""
import ctypes
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

def run(executable, arguments, text=""):
    return subprocess.run([executable] + arguments, input=text, text=True,
                          capture_output=True, timeout=4, creationflags=subprocess.CREATE_NO_WINDOW)

media, injector, network = sys.argv[1:]
try:
    for executable, extra, stdin in [(media, [], ""), (injector, [], ""),
                                     (network, ["--ssid", "WVD-Stop-Test"], "TestPassword123\n")]:
        result = run(executable, extra + ["--stop-event", name], stdin)
        assert result.returncode == 0, result.stderr
        assert "DIRECT_WIFI_STARTED" not in result.stdout
    for ssid, password in [("", "TestPassword123"), ("X" * 33, "TestPassword123"),
                           ("WVD-Test", "short"), ("WVD-Test", "x" * 64)]:
        result = run(network, ["--ssid", ssid], password + "\n")
        assert result.returncode == 2, (ssid, result.returncode, result.stderr)
        assert "DIRECT_WIFI_STARTED" not in result.stdout
    print("launcher stop events and offline-network argument validation passed; radio unchanged")
finally:
    kernel.CloseHandle(event)
