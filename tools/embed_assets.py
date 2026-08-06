#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "assets_embedded.h"
ASSETS = {
    "user_chrome_css": ROOT / "assets" / "userChrome.css",
    "user_content_css": ROOT / "assets" / "userContent.css",
}

lines = [
    "#ifndef BB_PYWALFOX_ASSETS_EMBEDDED_H",
    "#define BB_PYWALFOX_ASSETS_EMBEDDED_H",
    "",
]
for name, path in ASSETS.items():
    data = path.read_bytes()
    lines.append(f"static const unsigned char {name}[] = {{")
    for offset in range(0, len(data), 12):
        chunk = ", ".join(f"0x{value:02x}" for value in data[offset:offset + 12])
        lines.append(f"    {chunk},")
    lines.extend([
        "};",
        f"static const size_t {name}_length = sizeof({name});",
        "",
    ])
lines.extend(["#endif", ""])
OUTPUT.write_text("\n".join(lines), encoding="utf-8")
