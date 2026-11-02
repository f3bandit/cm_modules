# SS1 Console Mode for Windows

The PC app for Console Mode on the SuperStation One. Its window looks like Console Mode on the TV (the same
layout, Console Mode's own font, fetched from the SS1), and it talks to the SS1 through the app link
(`../remote`, ss1remote). Not SS1 Link: that one is for debugging and testing.

**The base (0.1.0):** only what Console Mode itself has. Find the SS1 on the network or type its address, pair
with the code Console Mode shows, then Home (Load Game, History, Favorites, Settings), Load Game (groups,
systems, folders and games as Console Mode lists them, box art on the right), Search (X), and A on a game
starts it on the TV. Keyboard, mouse or a controller plugged into the PC.

**Built from Phosphor for Windows** (phosphor_os/apps/windows, also f3bandit's): the same window (Edge in app
mode), tray icon, pairing, reconnecting when the SS1's address changes. Features come over one at a time,
each in its own file adding its routes from `init()` (`features` in server.go) and its screens to the web app.

| File | What |
|---|---|
| `main.go` | start-up: the local server, the window, the tray |
| `server.go` | the base routes: state, discover, pair, forget, reconnect, call, art, font |
| `console.go` | requests to the SS1 (pairing, the key, busy retries) |
| `discover.go`, `reconnect.go` | finding the SS1 on the network, following it to a new address |
| `config.go`, `secret_*.go` | settings in `%APPDATA%\SS1 Console Mode`, the key protected with Windows' DPAPI |
| `window_*.go`, `tray_windows.go`, `ui.go` | the window and the tray icon |
| `web/` | the screens (`app.js`), Console Mode's look (`app.css`) |

**Build:** `sh build-exe.sh` (Go 1.26+) makes `dist/SS1ConsoleMode.exe`, one exe, no install.

**Features from Phosphor for Windows** come one at a time, each agreed first. Candidates: box art and details
lookups on the PC, a game's details page, Update All (all its settings, without Phosphor's guards: only the
MiSTer/Linux protection, the restore points, stays), MiSTer & Linux backups, the SD card tools,
notifications, updating itself.
