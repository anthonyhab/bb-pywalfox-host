#!/usr/bin/env python3
import json
import os
import select
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path

HOST = os.environ["HOST_BINARY"]


def write_colors(path: Path, first: str = "#101010") -> None:
    colors = {f"color{i}": f"#{i:06x}" for i in range(16)}
    colors["color0"] = first
    document = {
        "special": {"background": first, "foreground": colors["color15"]},
        "colors": colors,
        "wallpaper": 'wallpaper "quoted"\\name.png',
    }
    write_document(path, document)


def write_document(path: Path, document: dict) -> None:
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(document), encoding="utf-8")
    os.replace(temporary, path)


def write_raw(path: Path, source: str) -> None:
    temporary = path.with_suffix(".tmp")
    temporary.write_text(source, encoding="utf-8")
    os.replace(temporary, path)


def frame(message) -> bytes:
    payload = json.dumps(message, separators=(",", ":")).encode()
    return struct.pack("=I", len(payload)) + payload


def send(process: subprocess.Popen, message: dict) -> None:
    process.stdin.write(frame(message))
    process.stdin.flush()


def send_many(process: subprocess.Popen, messages) -> None:
    process.stdin.write(b"".join(frame(message) for message in messages))
    process.stdin.flush()


def read_exact(stream, length: int) -> bytes:
    chunks = []
    remaining = length
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            raise AssertionError("native host closed its output early")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def receive(process: subprocess.Popen, timeout: float = 1.0) -> dict:
    ready, _, _ = select.select([process.stdout], [], [], timeout)
    if not ready:
        raise AssertionError("timed out waiting for native host response")
    length = struct.unpack("=I", read_exact(process.stdout, 4))[0]
    return json.loads(read_exact(process.stdout, length))


def assert_no_message(process: subprocess.Popen, timeout: float = 0.15) -> None:
    ready, _, _ = select.select([process.stdout], [], [], timeout)
    if ready:
        raise AssertionError(f"unexpected native message: {receive(process, 0)}")


def stop_process(process: subprocess.Popen) -> None:
    process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)
    stderr = process.stderr.read().decode(errors="replace")
    if stderr:
        print(stderr, end="", file=os.sys.stderr)


def test_self_healing_watch(root: Path, base_environment: dict) -> None:
    colors_path = root / "late" / "wal" / "colors.json"
    socket_path = root / "late.sock"
    environment = base_environment.copy()
    environment["PYWALFOX_COLORS_PATH"] = str(colors_path)
    environment["PYWALFOX_SOCKET_PATH"] = str(socket_path)

    process = subprocess.Popen(
        [HOST, "moz-extension://pywalfox-test/"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
        env=environment,
    )
    try:
        assert_no_message(process, 0.1)
        colors_path.parent.mkdir(parents=True)
        write_colors(colors_path, "#334455")
        assert receive(process, 1.0)["data"]["colors"][0] == "#334455"

        old_directory = colors_path.parent.with_name("wal-old")
        colors_path.parent.rename(old_directory)
        colors_path.parent.mkdir()
        write_colors(colors_path, "#556677")
        assert receive(process, 1.0)["data"]["colors"][0] == "#556677"
    finally:
        stop_process(process)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="pywalfox-host-test-") as directory:
        root = Path(directory)
        colors_path = root / "colors.json"
        socket_path = root / "host.sock"
        profile_path = root / "firefox-profile"
        chrome_path = profile_path / "chrome"
        chrome_path.mkdir(parents=True)
        user_chrome_path = chrome_path / "userChrome.css"
        user_chrome_path.write_text("/* preserved custom CSS */\n", encoding="utf-8")
        write_colors(colors_path)

        environment = os.environ.copy()
        environment["PYWALFOX_COLORS_PATH"] = str(colors_path)
        environment["PYWALFOX_SOCKET_PATH"] = str(socket_path)
        environment["PYWALFOX_PROFILE_PATH"] = str(profile_path)
        environment["ASAN_OPTIONS"] = "detect_leaks=0"

        install_home = root / "install-home"
        (install_home / ".mozilla").mkdir(parents=True)
        install_environment = environment.copy()
        install_environment["HOME"] = str(install_home)
        subprocess.run([HOST, "install"], env=install_environment, check=True, capture_output=True)
        manifest = json.loads((install_home / ".mozilla/native-messaging-hosts/pywalfox.json").read_text())
        assert manifest["allowed_extensions"] == [
            "pywalfox@bb.hab.rip",
            "pywalfox@frewacom.org",
        ]

        process = subprocess.Popen(
            [HOST, "moz-extension://pywalfox-test/"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
            env=environment,
        )
        try:
            assert_no_message(process)

            send(process, {"action": "debug:version"})
            version = receive(process)
            assert version == {
                "action": "debug:version",
                "success": True,
                "data": "2.7.4",
            }

            send_many(process, [
                {"action": "action:colors"},
                {"action": "debug:version"},
            ])
            colors = receive(process)
            assert colors["action"] == "action:colors"
            assert colors["success"] is True
            assert colors["data"]["colors"][0] == "#101010"
            assert colors["data"]["wallpaper"] == 'wallpaper "quoted"\\name.png'
            assert receive(process) == {
                "action": "debug:version",
                "success": True,
                "data": "2.7.4",
            }

            write_colors(colors_path, "#101010")
            assert_no_message(process)

            complete_colors = {f"color{i}": f"#{i:06x}" for i in range(16)}
            malformed_documents = [
                # Every key is present, but the document is truncated.
                '{"colors":' + json.dumps(complete_colors),
                # Valid JSON with all keys in the wrong object.
                json.dumps({"special": complete_colors}),
                # Duplicate relevant keys must not be resolved by last-one-wins.
                json.dumps({"colors": complete_colors}).replace(
                    '"color0": "#000000"',
                    '"color0": "#000000", "color0": "#ffffff"',
                    1,
                ),
            ]
            for malformed_source in malformed_documents:
                write_raw(colors_path, malformed_source)
                assert_no_message(process)
                send(process, {"action": "action:colors"})
                malformed = receive(process)
                assert malformed["action"] == "action:colors"
                assert malformed["success"] is False
                # Invalid input must not replace the last-good dedupe state.
                write_colors(colors_path, "#101010")
                assert_no_message(process)

            started = time.monotonic()
            write_colors(colors_path, "#abcdef")
            changed = receive(process)
            elapsed_ms = (time.monotonic() - started) * 1000
            assert changed["data"]["colors"][0] == "#abcdef"
            assert elapsed_ms < 500, elapsed_ms
            assert_no_message(process, 0.05)

            subprocess.run([HOST, "update"], env=environment, check=True)
            assert_no_message(process)

            stalled = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            stalled.connect(str(socket_path))
            stalled.sendall(b"\x10")
            write_colors(colors_path, "#123456")
            unstalled = receive(process, 0.5)
            assert unstalled["data"]["colors"][0] == "#123456"
            stalled.close()

            send(process, {"action": "css:enable", "target": "userChrome"})
            enabled = receive(process)
            assert enabled["action"] == "css:enable"
            assert enabled["success"] is True
            enabled_css = user_chrome_path.read_text(encoding="utf-8")
            assert "/* preserved custom CSS */" in enabled_css
            assert "BEGIN PYWALFOX CUSTOM CSS" in enabled_css

            send(process, {"action": "css:font:size", "target": "userChrome", "size": 17})
            resized = receive(process)
            assert resized["action"] == "css:font:size"
            assert resized["success"] is True
            assert resized["data"] == 17
            assert "--pywalfox-font-size: 17px;" in user_chrome_path.read_text(encoding="utf-8")

            send(process, {"action": "css:enable", "target": "../escape"})
            rejected = receive(process)
            assert rejected["action"] == "css:enable"
            assert rejected["success"] is False
            assert not (profile_path / "escape.css").exists()

            send(process, {"action": "css:disable", "target": "userChrome"})
            disabled = receive(process)
            assert disabled["action"] == "css:disable"
            assert disabled["success"] is True
            disabled_css = user_chrome_path.read_text(encoding="utf-8")
            assert "/* preserved custom CSS */" in disabled_css
            assert "PYWALFOX CUSTOM CSS" not in disabled_css

            send(process, {"action": "css:enable"})
            invalid = receive(process)
            assert invalid["action"] == "action:invalid"
            assert invalid["success"] is False

            print(
                "protocol, bounded socket clients, atomic inotify, dedup, safe CSS, and CLI update "
                f"passed ({elapsed_ms:.2f} ms)"
            )
        finally:
            stop_process(process)

        test_self_healing_watch(root, environment)
        test_boot_fallback_lifecycle(root, environment)
        test_profiles_ini_default_resolution(root)

def test_boot_fallback_lifecycle(root: Path, base_environment: dict) -> None:
    colors_path = root / "colors.json"
    socket_path = root / "fallback.sock"
    profile_path = root / "fallback-profile"
    profile_path.mkdir()
    write_colors(colors_path, "#101014")

    environment = base_environment.copy()
    environment["PYWALFOX_COLORS_PATH"] = str(colors_path)
    environment["PYWALFOX_SOCKET_PATH"] = str(socket_path)
    environment["PYWALFOX_PROFILE_PATH"] = str(profile_path)

    fallback = profile_path / "chrome" / "palette-boot.css"
    process = subprocess.Popen(
        [HOST, "moz-extension://pywalfox-test/"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
        env=environment,
    )
    try:
        deadline = time.monotonic() + 1.0
        while not fallback.exists():
            assert time.monotonic() < deadline, "fallback not written at spawn"
            time.sleep(0.02)
        css = fallback.read_text(encoding="utf-8")
        assert "invert(1) hue-rotate(180deg)" in css, "fallback must invert pages"
        assert "html:not([data-darkreader-scheme]) body" in css

        send(process, {"action": "debug:version"})
        assert receive(process, 1.0)["action"] == "debug:version"
        deadline = time.monotonic() + 1.0
        while fallback.exists():
            assert time.monotonic() < deadline, "fallback not removed after boot"
            time.sleep(0.02)

        process.stdin.close()
        process.wait(timeout=5)
        deadline = time.monotonic() + 1.0
        while not fallback.exists():
            assert time.monotonic() < deadline, "fallback not re-armed at shutdown"
            time.sleep(0.02)
        assert "invert(1)" in fallback.read_text(encoding="utf-8")
    finally:
        stop_process(process)




def test_profiles_ini_default_resolution(root: Path) -> None:
    """The [Install*] Default is the profile DIRECTORY name: the resolver must
    match it against Path=, not Name=, and prefer it over a legacy Default=1
    section (which may belong to an unrelated old profile)."""
    home = root / "home"
    firefox_dir = home / ".mozilla" / "firefox"
    firefox_dir.mkdir(parents=True)
    (firefox_dir / "profiles.ini").write_text(
        "[Install4F96D1932A9F858E]\n"
        "Default=3ix5m4nz.default-release\n"
        "Locked=1\n\n"
        "[Profile1]\n"
        "Name=default\n"
        "Path=7aybxa48.default\n"
        "Default=1\n\n"
        "[Profile0]\n"
        "Name=default-release\n"
        "Path=3ix5m4nz.default-release\n",
        encoding="utf-8",
    )
    (firefox_dir / "7aybxa48.default").mkdir()
    (firefox_dir / "3ix5m4nz.default-release").mkdir()
    wal_dir = home / ".cache" / "wal"
    wal_dir.mkdir(parents=True)
    write_colors(wal_dir / "colors.json", "#123456")

    environment = os.environ.copy()
    environment["HOME"] = str(home)
    environment.pop("PYWALFOX_PROFILE_PATH", None)
    environment.pop("PYWALFOX_COLORS_PATH", None)
    environment["PYWALFOX_SOCKET_PATH"] = str(root / "ini.sock")

    process = subprocess.Popen(
        [HOST, "start"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
        env=environment,
    )
    try:
        process.stdin.close()
        process.wait(timeout=5)
        expected = firefox_dir / "3ix5m4nz.default-release" / "chrome" / "palette-boot.css"
        assert expected.exists(), "fallback must land in the install-declared profile"
        assert "invert(1)" in expected.read_text(encoding="utf-8")
        wrong = firefox_dir / "7aybxa48.default" / "chrome" / "palette-boot.css"
        assert not wrong.exists(), "must not pick the legacy Default=1 profile"
    finally:
        stop_process(process)


if __name__ == "__main__":
    main()
