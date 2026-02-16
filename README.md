# bb-pywalfox-host

> Lightweight C native messaging host for Pywalfox

A drop-in replacement for pywalfox's Python/Go host with **~1MB memory usage** instead of ~10MB.

---

## Why?

The original Pywalfox host uses Python or Go and consumes ~10MB RAM. This C version uses **~1MB** - 10x less memory.

Perfect if you run multiple color watchers (Dark Reader, Pywalfox, etc.) and want to minimize footprint.

---

## Requirements

- Linux with inotify support
- [Pywalfox extension](https://addons.mozilla.org/en-US/firefox/addon/pywalfox/) for Firefox
- pywal or similar color generator

---

## Quick Start

### Arch Linux (AUR)
```bash
yay -S bb-pywalfox-host
bb-pywalfox-host install
# Restart Firefox
```

### Manual Build
```bash
gcc -O2 -s -o bb-pywalfox-host bb-pywalfox-host.c
sudo install -Dm755 bb-pywalfox-host /usr/bin/bb-pywalfox-host
bb-pywalfox-host install
```

---

## Usage

### CLI Commands

| Command | Description |
|---------|-------------|
| `bb-pywalfox-host` | Run daemon (auto-started by Firefox) |
| `bb-pywalfox-host install` | Install native messaging manifest |
| `bb-pywalfox-host uninstall` | Remove native messaging manifest |
| `bb-pywalfox-host update` | Trigger color reload (via socket) |
| `bb-pywalfox-host dark` | Switch to dark theme mode |
| `bb-pywalfox-host light` | Switch to light theme mode |
| `bb-pywalfox-host auto` | Switch to auto theme mode |
| `bb-pywalfox-host log` | Open daemon log in $EDITOR |
| `bb-pywalfox-host --help` | Show help |

---

## How It Works

1. **Firefox starts the daemon** via native messaging when Pywalfox extension loads
2. **Socket server** runs at `/tmp/pywalfox_socket` for CLI commands
3. **inotify watches** `~/.cache/wal/colors.json` for file changes
4. **Automatic updates** sent to Firefox when colors change (no manual action needed)

### Architecture

- **Native Messaging**: Communication with Firefox extension via stdin/stdout
- **Socket**: CLI commands (`update`, `dark`, `light`, etc.) connect via Unix socket
- **inotify**: File system monitoring for automatic color updates

---

## Integration with Theme Hooks

The daemon automatically updates when `~/.cache/wal/colors.json` changes. No manual `update` call needed in your theme hooks.

If using theme hooks that call `pywalfox update` or similar, you can remove those calls - inotify handles it automatically.

---

## Stats

| Metric | Value |
|--------|-------|
| Binary size | ~22KB |
| Memory usage | ~1MB |
| Dependencies | None (glibc only) |
| Build tools | gcc |

---

## Troubleshooting

### Daemon not running
```bash
# Check if daemon is running
ps aux | grep bb-pywalfox-host

# Start manually (for debugging)
bb-pywalfox-host
```

### Socket issues
```bash
# Check socket exists
ls -la /tmp/pywalfox_socket

# Kill stale daemon
pkill bb-pywalfox-host
```

### Extension not receiving colors
```bash
# Reinstall manifest
bb-pywalfox-host uninstall
bb-pywalfox-host install
# Restart Firefox
```

---

## Building from Source

```bash
# Clone and build
git clone https://github.com/anthonyhab/bb-pywalfox-host.git
cd bb-pywalfox-host
gcc -O2 -s -o bb-pywalfox-host bb-pywalfox-host.c

# Install
sudo install -Dm755 bb-pywalfox-host /usr/bin/bb-pywalfox-host
bb-pywalfox-host install
```

---

## License

MIT - See [LICENSE](LICENSE)
