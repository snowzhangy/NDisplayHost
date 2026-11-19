"""Real helper/pipe lifecycle test. Sends zero events; never injects user input."""
import os
import struct
import subprocess
import sys
import time

TOKEN = 0x123456789ABCDEF
pipe = rf"\\.\pipe\wvd.input.test.{os.getpid()}"
process = subprocess.Popen([sys.argv[1], "--pipe", pipe, "--token", f"{TOKEN:X}"],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           creationflags=subprocess.CREATE_NO_WINDOW)

def connect():
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            return open(pipe, "r+b", buffering=0)
        except OSError:
            if process.poll() is not None:
                raise RuntimeError(process.communicate()[1].decode(errors="replace"))
            time.sleep(0.03)
    raise TimeoutError("input helper pipe unavailable")

def read_exact(stream, count):
    data = bytearray()
    while len(data) < count:
        chunk = stream.read(count - len(data))
        if not chunk:
            raise EOFError("pipe closed")
        data.extend(chunk)
    return data

def request(stream, command, extra=b"", token=TOKEN):
    body = struct.pack("<IIQ", command, 1, token) + extra
    stream.write(struct.pack("<IHHIQ", 0x50445657, 1, 32, len(body), 0) + body)
    magic, version, kind, length, _ = struct.unpack("<IHHIQ", read_exact(stream, 20))
    assert (magic, version, kind, length) == (0x50445657, 1, 33, 32)
    return struct.unpack("<IIQQQ", read_exact(stream, length))

try:
    with connect() as owner:
        assert request(owner, 20, token=0)[1] & 4
        armed = request(owner, 20)
        assert armed[0] == 1 and armed[4] != 0
        generation = armed[4]
        # A diagnostic connection closing cannot revoke the armed connection.
        with connect() as diagnostic:
            assert request(diagnostic, 1)[0] == 1
        time.sleep(0.05)
        assert request(owner, 24)[0] == 1
        assert request(owner, 22, struct.pack("<QQI", generation + 1, 1, 0))[1] & 2
        assert request(owner, 22, struct.pack("<QQI", generation - 1, 2, 0))[1] & 2
        # Wait for attachment. On an unlocked interactive desktop, a zero-event
        # batch verifies the command/body offsets without moving/typing anything.
        deadline = time.monotonic() + 3
        state = request(owner, 24)
        while state[1] & 8 and time.monotonic() < deadline:
            time.sleep(0.03)
            state = request(owner, 24)
        if not state[1] & 8:
            assert not request(owner, 22, struct.pack("<QQI", generation, 3, 0))[1] & 2
        else:
            print("session suspended: acceptance check skipped")
        assert request(owner, 22)[1] & 2  # malformed batch
    with connect() as observer:
        deadline = time.monotonic() + 2
        state = request(observer, 1)
        while state[0] and time.monotonic() < deadline:
            time.sleep(0.01)
            state = request(observer, 1)
        assert state[0] == 0 and state[4] > generation
        rearmed = request(observer, 20)
        assert rearmed[0] == 1 and rearmed[4] > generation
        disarmed = request(observer, 21)
        assert disarmed[0] == 0 and disarmed[4] > rearmed[4]
    print("input IPC auth, generations, offsets and disconnect regressions passed")
finally:
    process.terminate()
    process.communicate(timeout=5)
