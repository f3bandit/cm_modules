# Console Mode for Windows (cm_win)

The PC app of Console Mode, Taki's ecosystem across his devices: Console Mode on the SuperStation One's TV, and this one on Windows. Its window looks like Console Mode on the TV (the same
layout, Console Mode's own font, fetched from the SS1), and it talks to the SS1 through the app link
(`../remote`, ss1remote). Not SS1 Link: that one is for debugging and testing.

**What it is:** Phosphor for Windows' tools made for Console Mode, in Console Mode's look (its font, the white bar,
the status line and button hints), not a copy of Console Mode's screens. Home is the list of tools; each one
comes over from Phosphor for Windows on its own. Here now: **Box art & details** (your games as Console Mode
lists them, box art from a file, details for gamelist.xml) and **Connection & about** (pairing). Keyboard,
mouse or a controller plugged into the PC.

**Built from Phosphor for Windows** (phosphor_os/apps/windows, also f3bandit's): the same window (Edge in app
mode), tray icon, pairing, reconnecting when the SS1's address changes. Features come over one at a time,
each in its own file adding its routes from `init()` (`features` in server.go) and its screens to the web app.

| File | What |
|---|---|
| `main.go` | start-up: the local server, the window, the tray |
| `server.go` | the base routes: state, discover, pair, forget, reconnect, call, art, font |
| `console.go` | requests to the SS1 (pairing, the key, busy retries) |
| `discover.go`, `reconnect.go` | finding the SS1 on the network, following it to a new address |
| `config.go`, `secret_*.go` | settings in `%APPDATA%\Console Mode`, the key protected with Windows' DPAPI |
| `window_*.go`, `tray_windows.go`, `ui.go` | the window and the tray icon |
| `web/` | the screens (`app.js`), Console Mode's look (`app.css`) |

**Build:** `sh build-exe.sh` (Go 1.26+) makes `dist/cm_win.exe`, one exe, no install.

**Features from Phosphor for Windows** come one at a time, each agreed first. Candidates: box art and details
lookups on the PC, a game's details page, Update All (all its settings, without Phosphor's guards: only the
MiSTer/Linux protection, the restore points, stays), MiSTer & Linux backups, the SD card tools,
notifications, updating itself.
