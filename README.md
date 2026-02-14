# bb-pywalfox-host

> Lightweight C native messaging host for Pywalfox

A drop-in replacement for pywalfox's Python/Go host with **~1MB memory usage** instead of ~10MB.

---

## Why?

The original Pywalfox host uses Python or Go and consumes ~10MB RAM. This C version uses **~1MB** - 10x less memory.

Perfect if you run multiple color watchers (Dark Reader, Pywalfox, etc.) and want to minimize footprint.

---

## Quick Start

### 1. Install bb-pywalfox-host

**Arch Linux (AUR):**
```bash
yay -S bb-pywalfox-host
bb-pywalfox-host --setup
# Restart Firefox
```

### 2. Or build manually:
```bash
gcc -O2 -s -o bb-pywalfox-host bb-pywalfox-host.c
sudo install -Dm755 bb-pywalfox-host /usr/bin/bb-pywalfox-host
bb-pywalfox-host --setup
```

---

## How It Works

- Watches `~/.cache/wal/colors` using Linux inotify
- Parses pywal's JSON output
- Sends theme updates to Pywalfox via Firefox native messaging
- **~15KB binary, ~1MB RAM**

---

## Commands

```bash
bb-pywalfox-host --setup    # Create native messaging config
bb-pywalfox-host            # Run daemon (auto-started by Firefox)
```

---

## License

MIT - See [LICENSE](LICENSE)
