#!/usr/bin/env python3
"""Emit-latency benchmark for bb-pywalfox-host.

Measures wall-clock time from colors.json write completion to the host's
framed native-messaging message arriving on stdout, for three write
patterns:

  WAL_IN_PLACE  pywal's actual pattern: open('w') + write + close (30x)
  WAL_RENAME    atomic replace: write temp file + os.rename over (30x)
  SLOW_WRITE    truncate + two-chunk write with a 50ms gap (10x)

The host watches the colors directory with inotify (IN_CLOSE_WRITE |
IN_MOVED_TO | IN_CREATE) and emits one deduplicated framed message per
change. Timestamps use time.monotonic_ns; t0 = close/rename completion,
t1 = full framed message read. Any frame beyond the expected one is an
"extra"; MISS = no frame within the 2s bound; error frames = action:colors
with success:false. A reader thread captures frame arrival times during the
slow phase so emit timing is measured while the writer is still sleeping.

Stdlib only. Usage:
    HOST_BINARY=/path/to/bb-pywalfox-host python3 bench/bench_emit_latency.py
"""

import json
import math
import os
import select
import struct
import subprocess
import tempfile
import threading
import time
from pathlib import Path

HOST = os.environ.get("HOST_BINARY") or str(
    Path(__file__).resolve().parent.parent / "bb-pywalfox-host"
)
ITERATIONS = 30
SLOW_ITERATIONS = 10
WAIT_TIMEOUT = 2.0
QUIET_PEEK = 0.03
CHUNK_SLEEP = 0.05


def make_palette(seed):
    """A valid pywal colors.json document; seed varies color0/background."""
    background = f"#{seed:06x}"
    colors = {f"color{i}": f"#{seed + i:06x}" for i in range(16)}
    return {
        "special": {"background": background, "foreground": colors["color15"]},
        "colors": colors,
        "wallpaper": "bench-wallpaper.png",
    }


def write_in_place(path, document):
    with open(path, "w", encoding="utf-8") as file:
        file.write(json.dumps(document))


def write_atomic(path, document):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(document), encoding="utf-8")
    os.rename(temporary, path)


def frame(message):
    payload = json.dumps(message, separators=(",", ":")).encode()
    return struct.pack("=I", len(payload)) + payload


def read_exact(stream, length, timeout):
    chunks = []
    remaining = length
    deadline = time.monotonic() + timeout
    while remaining > 0:
        now = time.monotonic()
        if now >= deadline:
            raise TimeoutError
        ready, _, _ = select.select([stream], [], [], deadline - now)
        if not ready:
            raise TimeoutError
        chunk = stream.read(remaining)
        if not chunk:
            raise EOFError("host closed stdout")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_frame(process, timeout):
    """Return the next framed native-messaging dict, or None on timeout."""
    try:
        header = read_exact(process.stdout, 4, timeout)
    except (TimeoutError, EOFError):
        return None
    (length,) = struct.unpack("=I", header)
    try:
        payload = read_exact(process.stdout, length, timeout)
    except (TimeoutError, EOFError):
        return None
    return json.loads(payload)


def classify(message):
    if message.get("action") == "action:colors":
        return "success" if message.get("success") is True else "error"
    return "other"


def label(message):
    return message.get("action", "?")


def percentile(sorted_values, p):
    index = int(math.ceil(p / 100.0 * len(sorted_values))) - 1
    index = max(0, min(index, len(sorted_values) - 1))
    return sorted_values[index]


def print_stats(name, values, misses, errors, extras, others):
    present = [v for v in values if v is not None]
    if not present:
        print(
            f"{name}: n=0 NO FRAMES | misses={misses} "
            f"error-frames={errors} extra-frames={extras} other-frames={others}"
        )
        return
    ordered = sorted(present)
    print(
        f"{name}: n={len(present)} min={ordered[0] / 1e6:.1f} "
        f"p50={percentile(ordered, 50) / 1e6:.1f} "
        f"p95={percentile(ordered, 95) / 1e6:.1f} "
        f"max={ordered[-1] / 1e6:.1f} ms "
        f"| misses={misses} error-frames={errors} "
        f"extra-frames={extras} other-frames={others}"
    )


def drain_quiet(process):
    """Count any frames that arrive within QUIET_PEEK; returns the count."""
    count = 0
    while True:
        extra = read_frame(process, QUIET_PEEK)
        if extra is None:
            return count
        count += 1


def run_phase(process, colors_path, name, pattern):
    deltas = []
    misses = 0
    errors = 0
    extras = 0
    others = 0
    for iteration in range(ITERATIONS):
        document = make_palette(iteration + 1)
        t0 = time.monotonic_ns()
        if pattern == "in_place":
            write_in_place(colors_path, document)
        else:
            write_atomic(colors_path, document)

        message = read_frame(process, WAIT_TIMEOUT)
        if message is None:
            misses += 1
            extras += drain_quiet(process)  # late frame, if any
            continue

        delta = time.monotonic_ns() - t0
        kind = classify(message)
        expected = document["special"]["background"]
        if kind == "success" and message.get("data", {}).get("colors", [None])[0] == expected:
            deltas.append(delta)
        elif kind == "error":
            errors += 1
        else:
            others += 1
        extras += drain_quiet(process)
    print_stats(name, deltas, misses, errors, extras, others)


def run_slow_phase(process, colors_path):
    sequences = {}
    first_from_open = []
    success_from_close = []
    errors = extras = others = 0

    for iteration in range(SLOW_ITERATIONS):
        document = make_palette(200 + iteration)
        expected = document["special"]["background"]
        frames = []
        stop = threading.Event()

        def reader():
            while not stop.is_set():
                message = read_frame(process, 0.05)
                if message is not None:
                    frames.append((time.monotonic_ns(), classify(message), message))

        t_open = time.monotonic_ns()
        colors_path.unlink(missing_ok=True)
        thread = threading.Thread(target=reader, daemon=True)
        thread.start()
        with open(colors_path, "w", encoding="utf-8") as file:
            source = json.dumps(document)
            half = len(source) // 2
            file.write(source[:half])
            file.flush()
            time.sleep(CHUNK_SLEEP)
            file.write(source[half:])
        t_close = time.monotonic_ns()
        time.sleep(0.05)  # let the close-triggered frame land
        stop.set()
        thread.join(timeout=1)

        kinds = [frame[1] for frame in frames]
        errors += kinds.count("error")
        others += sum(1 for kind in kinds if kind == "other")
        for _, kind, message in frames:
            if kind == "success" and message.get("data", {}).get("colors", [None])[0] != expected:
                others += 1  # stale or misattributed success frame

        key = ",".join(kinds) if kinds else "(none)"
        sequences[key] = sequences.get(key, 0) + 1
        if frames:
            first_from_open.append(frames[0][0] - t_open)
        success_frame = next(
            (arrival for arrival, kind, _ in frames if kind == "success"), None
        )
        if success_frame is not None:
            success_from_close.append(success_frame - t_close)
        detail = " ".join(
            f"{kind}[{label(message)}]@+{(arrival - t_open) / 1e6:.2f}ms"
            for arrival, kind, message in frames
        ) if frames else "no frame"
        print(f"  slow #{iteration + 1:02d}: frames=[{key}] {detail}")

    print(f"SLOW_WRITE sequences: {sequences}")
    print_stats(
        "SLOW_WRITE first-frame (from open)",
        first_from_open, 0, errors, extras, others,
    )
    print_stats(
        "SLOW_WRITE success-frame (from close)",
        success_from_close, 0, errors, extras, others,
    )


def main():
    print("# bb-pywalfox-host emit-latency benchmark")
    print(f"host: {HOST}")

    with tempfile.TemporaryDirectory(prefix="pywalfox-host-bench-") as directory:
        root = Path(directory)
        colors_path = root / "colors.json"
        write_in_place(colors_path, make_palette(0))

        environment = os.environ.copy()
        environment["PYWALFOX_COLORS_PATH"] = str(colors_path)
        environment["PYWALFOX_SOCKET_PATH"] = str(root / "host.sock")
        environment["PYWALFOX_PROFILE_PATH"] = str(root / "profile")

        process = subprocess.Popen(
            [HOST, "moz-extension://pywalfox-bench/"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
            env=environment,
        )
        try:
            # Handshake exactly as the extension does (see tests/test_protocol.py).
            process.stdin.write(frame({"action": "debug:version"}))
            process.stdin.flush()
            version = read_frame(process, WAIT_TIMEOUT)
            if not version or version.get("action") != "debug:version" or not version.get("success"):
                raise AssertionError(f"handshake failed: {version!r}")
            print(f"handshake: debug:version -> {version.get('data')!r}")

            stray = read_frame(process, 0.1)
            if stray is not None:
                raise AssertionError(f"unexpected frame at startup: {stray!r}")
            print("startup: no spontaneous frames")

            run_phase(process, colors_path, "WAL_IN_PLACE (30x open('w') rewrite)", "in_place")
            run_phase(process, colors_path, "WAL_RENAME (30x temp+os.rename)", "rename")
            run_slow_phase(process, colors_path)
        finally:
            process.stdin.close()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=2)
            stderr = process.stderr.read().decode(errors="replace")
            if stderr:
                print(f"host stderr: {stderr!r}")


if __name__ == "__main__":
    main()
