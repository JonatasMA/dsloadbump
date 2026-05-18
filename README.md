# DSload v2 — RoMM Integration Fork

> **Fork by [VXisto](https://github.com/VXisto/dsloadbump)** — original DSload
> by [Lameguy64](https://github.com/Lameguy64) / Meido-Tek Productions (2016).
> The original project has been inactive since ~2016; this fork extends it with
> RoMM integration, DSi WiFi support, SD-card caching, ZIP extraction, and
> TwilightMenu++ directory layout.

DSload v2 is a Nintendo DS homebrew utility that lets you transfer ROMs and
files to your DS over WiFi from two sources:

1. **Server Mode** — original behaviour: a PC-side tool pushes files to the DS
   over a custom TCP protocol (unchanged from v0.25).

2. **RoMM Download Mode** — new in v2: the DS pulls ROMs directly from a
   self-hosted [RoMM](https://github.com/rommapp/romm) server via its HTTP/JSON
   API.  No FTP, NFS mount, or PC involvement required — just plain HTTP over
   your local WiFi network.

---

## Supported platforms (RoMM mode)

| Platform | RoMM `fs_slug` |
|---|---|
| Nintendo DS | `nds` |
| Game Boy Advance | `gba` |
| Game Boy / Color | `gb` / `gbc` |
| Mega Drive / Genesis | `genesis` / `megadrive` |
| Super Nintendo | `snes` |

---

## Quick-start

### 1 — Prerequisites

* A Nintendo DS / DS Lite with a DLDI-compatible flash card (R4, DSTT, etc.)
* DS WFC settings already configured for your router
* A running RoMM instance reachable via plain **HTTP** on your LAN
  (HTTPS is not supported — use a local HTTP port or reverse proxy without TLS)

### 2 — SD card layout

```
SD:/
├── dsload.nds       ← homebrew binary
└── romm.cfg         ← RoMM connection settings (see below)
```

### 3 — `romm.cfg` reference

Create a plain-text file `/romm.cfg` on the root of your SD card:

```ini
# RoMM server hostname or IP address (required)
server=192.168.1.100

# HTTP port (default: 3000)
port=3000

# API key — preferred auth (generate one in RoMM → Settings → API keys)
api_key=your_api_key_here

# Alternative: HTTP Basic auth
# username=admin
# password=yourpassword

# Destination directory on the SD card (default: /roms)
download_dir=/roms
```

> **Authentication priority:** `api_key` takes precedence.  If absent,
> `username`/`password` is sent as an HTTP Basic-auth header.

### 4 — Usage

1. Boot `dsload.nds` from your flash card menu.
2. The app initialises the file system and connects to your router.
3. A menu appears on the top screen:
   * **A** — Server Mode (receive files pushed from a PC)
   * **B** — RoMM Download Mode
   * **START** — power off / return to loader

#### RoMM Download Mode flow

```
Select Platform  →  Browse ROMs (L/R to page)  →  Confirm  →  Download
```

| Button | Action |
|--------|--------|
| UP / DOWN | Navigate list |
| A | Select / confirm |
| B | Go back |
| L / R | Previous / next page |

Downloaded files are saved to `download_dir/<fs_name>`.
The destination directory is created automatically if it does not exist.

---

## Debug log

Every RoMM session appends a plain-text log to `/romm_debug.log` on the SD
card.  The log records:

* Config values loaded (excluding password)
* Every HTTP request URL and response status / Content-Length
* JSON parsing results (platforms and ROM entries found)
* Download sizes and errors

Inspect the log from your PC to diagnose any connectivity issues.

---

## PC tool (Server Mode)

The `pctool/` directory contains the original Windows command-line utility.
Build with MinGW / MSVC and run:

```
dsload -ip <DS_IP> [-noverify] [-shutdown] [-dir <path>] file1 [file2 ...]
```

---

## Building from source

Requires [devkitARM](https://devkitpro.org/) with the `nds-dev` packages.

```bash
export DEVKITARM=/path/to/devkitARM
make
```

Output: `dsload.nds`

---

## Planned features

- [ ] Launch downloaded NDS files directly from the app
- [ ] Search/filter ROMs by name within RoMM mode
- [ ] Multi-file / multi-disk ROM zip extraction
- [ ] Wildcard file sends (Server Mode)
- [ ] CRC-based skip for already-present files (Server Mode)

### TBD — Self-updater (SELECT key)

The updater infrastructure is designed but not yet operational.
The SELECT key in the main menu will show a placeholder screen pointing
to the GitHub releases page.

**Planned design:** the DS fetches a plain-text version manifest from a
local HTTP endpoint (no TLS — NDS hardware has no SSL stack):

```json
{"version":"2.01","path":"/dsload/dsloadbump.nds","notes":"..."}
```

The manifest and `.nds` binary are served from the same nginx instance
that proxies RoMM.  A one-liner `location /dsload/` block is all that
is needed on the server side.  Optional `romm.cfg` keys:

```ini
# All default to your RoMM server / port if omitted
update_host=192.168.1.100
update_port=8054
update_check_path=/dsload/version.json
update_install_path=/dsloadbump.nds
```

This will be wired up and tested in a future release.

---

## Credits

| Contributor | Role |
|---|---|
| **[Lameguy64](https://github.com/Lameguy64)** (Meido-Tek Productions) | Original DSload v0.25 — server mode, TCP protocol, PC tool |
| **[VXisto](https://github.com/VXisto)** | v2 fork — RoMM HTTP/JSON integration, DSi WiFi (slots 1–6), SD-card metadata cache, streaming JSON parser, chunked transfer decoding, ZIP extraction (zlib), TwilightMenu++ platform directories, progress bar, Linux pctool port, DSi-enhanced ROM header |
| **[RoMM project](https://github.com/rommapp/romm)** | API spec and self-hosted ROM manager this fork integrates with |

This project is licensed under the GNU General Public License v2 (GPL-2.0),
inherited from the original DSload codebase.
