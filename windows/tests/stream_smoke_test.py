"""Manual wire smoke test: real desktop by default, --synthetic for headless.

Launches media briefly, receives Annex B without storing screen pixels, checks
codec setup before media and requests an independently decodable recovery IDR.
Requires an interactive desktop for the default mode. No headset required.
"""
import argparse
import collections
import re
import socket
import struct
import subprocess
import sys
import time
import threading
import statistics
sys.stdout.reconfigure(encoding="utf-8")

parser = argparse.ArgumentParser()
parser.add_argument("media")
parser.add_argument("--synthetic", action="store_true")
parser.add_argument("--codec", default="auto")
parser.add_argument("--monitor", help="exact GDI name or monitor interface to capture")
parser.add_argument("--connect", type=int, help="test an existing host instead of launching one")
parser.add_argument("--bitrate", type=int, default=60000, help="encoder ceiling in kbps")
parser.add_argument("--fps", type=int, default=60)
args = parser.parse_args()
with socket.socket() as reservation:
    reservation.bind(("127.0.0.1", 0))
    port = reservation.getsockname()[1]
command = [args.media, "--listen", str(port), "--demo", "8", "--no-discovery", "--codec", args.codec,
           "--bitrate", str(args.bitrate), "--fps", str(args.fps)]
if args.synthetic:
    command.append("--synthetic")
if args.monitor:
    command.extend(["--monitor", args.monitor])
if args.connect:
    port = args.connect
    process = None
else:
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                               creationflags=subprocess.CREATE_NO_WINDOW)
log_lines = collections.deque(maxlen=1000)
reader = None
if process:
    # A full stderr pipe can block the worker and corrupt the timing under test.
    def drain_log():
        for line in process.stderr:
            log_lines.append(line.decode(errors="replace"))
    reader = threading.Thread(target=drain_log, daemon=True)
    reader.start()

def exact(stream, length):
    data = bytearray()
    while len(data) < length:
        chunk = stream.recv(length - len(data))
        if not chunk:
            raise EOFError("media worker disconnected")
        data.extend(chunk)
    return data

def send(stream, kind, body):
    stream.sendall(struct.pack("<IHHIQ", 0x50445657, 1, kind, len(body), 0) + body)

def nal_types(data, codec):
    chunks = re.split(b"\x00\x00\x00\x01|\x00\x00\x01", data)
    return {((part[0] >> 1) & 63) if codec == 0 else (part[0] & 31)
            for part in chunks[1:] if part}

counts = collections.Counter()
config = None
recovery_key = False
requested = False
arrivals = []
try:
    deadline = time.monotonic() + 4
    while True:
        try:
            stream = socket.create_connection(("127.0.0.1", port), timeout=2)
            break
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.05)
    with stream:
        stream.settimeout(5)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            magic, version, kind, length, _ = struct.unpack("<IHHIQ", exact(stream, 20))
            assert (magic, version) == (0x50445657, 1) and length < 16 * 1024 * 1024
            body = exact(stream, length)
            counts[kind] += 1
            if kind == 5:
                generation = struct.unpack_from("<Q", body)[0]
                codec = body[8]
                width, height = struct.unpack_from("<II", body, 12)
                csd_size = struct.unpack_from("<I", body, 40)[0]
                assert csd_size > 0 and len(body) == 44 + csd_size
                expected = {32, 33, 34} if codec == 0 else {7, 8}
                assert expected <= nal_types(body[44:], codec)
                config = generation, codec, width, height
            elif kind == 10:
                arrivals.append(time.perf_counter())
                assert config and counts[3], "media preceded layout/codec setup"
                assert struct.unpack_from("<Q", body, 16)[0] == config[0]
                flags, size = struct.unpack_from("<HI", body, 46)
                assert size and len(body) == 52 + size
                types = nal_types(body[52:], config[1])
                assert types, "expected Annex B access unit"
                if flags & 1:
                    assert ({32, 33, 34} if config[1] == 0 else {7, 8}) <= types
                    assert types & ({19, 20, 21} if config[1] == 0 else {5})
                    if requested:
                        recovery_key = True
                if not requested:
                    send(stream, 12, struct.pack("<QII", config[0], 2, 0))
                    requested = True
        assert counts[10] > 0 and recovery_key
    output = ""
    if process:
        process.wait(timeout=10)
        reader.join(timeout=2)
        output = "".join(log_lines)
        assert process.returncode == 0, output
    print(f"{config[2]}x{config[3]} codec={'HEVC' if config[1] == 0 else 'H.264'}: "
          f"{counts[10]} media frames; setup order, Annex B and recovery IDR passed")
    if len(arrivals) > 1:
        gaps = sorted((b - a) * 1000 for a, b in zip(arrivals, arrivals[1:]))
        p95 = gaps[round((len(gaps) - 1) * 0.95)]
        print(f"arrival spacing: p50={statistics.median(gaps):.2f}ms p95={p95:.2f}ms worst={max(gaps):.2f}ms")
    print(output)
finally:
    if process and process.poll() is None:
        process.terminate()
        process.wait(timeout=5)
        reader.join(timeout=2)
