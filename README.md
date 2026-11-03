# cm_modules

The SuperStation One's front-port and drive tools as modules for a front end such as Console Mode: the
**memory card manager**, the **SNAC controller tester** and the **offline drive repair**. Everything except
the screens: each module gives the front end data and actions, and the front end draws them.

The controller tester and the memory card manager are f3bandit's idea and design.

**Licence:** GPL-3.0-or-later (LICENSE). **Retro Remake and Taki Udon** have their own permission on top
(LICENSE-RETRO-REMAKE.md): use any of it in Console Mode or any product of theirs, closed source included,
without the GPL's conditions; the only condition is crediting f3bandit.

## Which is for which

| Folder | What it is | Who uses it |
|---|---|---|
| `include/`, `src/` | The modules: memory card manager, controller tester, drive repair (`cmmod.h`) | Console Mode, linked in |
| `tools/cmjobd.c` | Runs a memory card / controller job while Console Mode is closed | installed on the SuperStation |
| `tools/cmmod.c` | Every module function from the command line (reference and testing) | developers, on the SuperStation |
| `vendor/` | ps1_memcard_lib: card images, saves, icons, the card library | the memory card module |
| `scripts/` | The offline SD card repair | the drive repair module |
| `remote/` | **The app link** (cmremote): the phone and PC apps reach the game library over the home network | Console Mode, installed on the SuperStation |
| (private) | **Console Mode for Windows (cm_win)**: the PC app, over the app link: its own private repository, f3bandit/cm_win | end users, on a PC |
| `cm-link/` | **CM Link, for debugging and testing only**: lets Claude (or a command prompt) reach a SuperStation running Console Mode over SSH or the dock's debug port, press buttons, see the screen. Not part of Console Mode, and not what the apps use (see below) | developers, on a PC |

## The two links: one for the apps, one for debugging

There are two different "links" to a SuperStation. They don't overlap:

| | **App link** (remote access) | **CM Link** (`cm-link/`) |
|---|---|---|
| What for | The **apps**: the phone and PC apps manage the game library (games, box art, details) over the home network | **Debugging and testing**: a developer (or Claude) runs commands, copies files, reads the boot log, presses buttons and takes screenshots |
| Who uses it | End users, through an app | Developers only, never end users |
| Runs on | A service on the SuperStation that answers the apps | A PC program (plus the Claude plugin) |
| Connection | HTTP, port 8767, home network only, the app pairs once with a 6-digit code from the TV | SSH (root) or the dock's USB-C debug port (serial console) |
| Ships with Console Mode | Yes | No. It's a tool for working on Console Mode, not a part of it |
| Here | `remote/` (cmremote) | `cm-link/` |

CM Link's tools all say `[CM Link / Console Mode]` first.

- C11, one header (`include/cmmod.h`), usable from C++. No dependencies beyond libc and POSIX.
- `make test` runs the host tests; `make arm` builds `cmjobd`, `cmmod`, `cmremote`, `cmnvme`, `cmtrimfs`, `cmident` and `cmdisc` for the SuperStation (static armhf).
- Tested on a SuperStation One running Console Mode 1.2.5: backup, verify and write of a real card (Console Mode
  closed and came back by itself each time), the controller tester (a DualShock in port 1, Select+Start back),
  and the repair status with an existing repair drive.

## Backups: one folder

Every backup the modules and the apps make on the SuperStation goes in **`/media/fat/ConsoleMode/tools/backups`**, one
subfolder per kind (`CMOD_BACKUPS` in `cmmod.h`): `restore-points` (MiSTer's Main, menu core, kernel, Linux),
`linux-settings` (settings inside Linux kept for after a Linux update), `memory-cards` (the memory card library:
call `mclib_set_dir(CMOD_BK_MEMCARDS)`), `saves`, `controllers`. `{"cmd":"backups"}` lists them with their
sizes. On the PC, the app keeps its own in one folder too (Documents\Console Mode\Backups, or one the user
picks), with the same kinds.

## How it fits together

While one of the front-port cores runs, the front end doesn't: the core has the screen. So a job goes to
**`cmjobd`**, a small program that outlives the front end:

```
front end                         cmjobd                                   core
---------                         -------                                   ----
cmod_job_start(BACKUP, path)  -->  writes the job + card files and an MGL
(MiSTer closes the front end)     load_core <mgl>  ---------------------->  reads the card, progress
                                  follows the job file                 <--  "SMCP" ... "SMCR" report
                                  copies the backup to `path`
                                  load_core menu.rbf  (front end back)
front end starts again       <--  CMOD_RESULT: state=done, message=..., counts
cmod_job_result("message")
```

**For users and testers:** `make installer` builds `build/Setup_Console_Mode_App.sh` (attached to each release
here). Copy it to the SD card's `Scripts` folder and run it from Console Mode's Scripts menu: it checks itself,
keeps a copy of the tools it replaces (`ConsoleMode/tools/backups/tools/<date>`, the newest 3) and puts them back
if anything fails, keeps `remote.ini` and `update-guard.ini`, starts the app link now and at every boot, and
shows the pairing code. It never writes Console Mode's own files. After that, **Scripts › Pair_Console_Mode_App**
shows a new code whenever another PC or phone pairs.

Install (what Console Mode's installer would do):

| File | Where |
|---|---|
| `build/cmjobd` | `/media/fat/ConsoleMode/tools/cmjobd` |
| `scripts/sd_offline_repair.sh`, `scripts/sd_offline_repair_boot.sh` | `/media/fat/ConsoleMode/tools/` |
| `build/cmremote` | `/media/fat/ConsoleMode/tools/cmremote` |
| `build/cmnvme` | `/media/fat/ConsoleMode/tools/cmnvme` (the NVMe drive's temperature for the Diagnostics dashboard: the SuperStation's only temperature sensor) |
| `build/cmtrimfs` | `/media/fat/ConsoleMode/tools/cmtrimfs` (TRIM for exFAT drives, which MiSTer's exFAT driver can't do: `cmtrim.sh` runs it) |
| `build/cmdisc` | `/media/fat/ConsoleMode/tools/cmdisc` (a game disc in a USB CD/DVD drive played live: the disc as `.cue`/`.bin` files read off it as the game asks (FUSE), started with the right core through a standard `.mgl`; `status`, `play`, `watch`, `stop`, `eject`; works with any MiSTer main program, nothing in it changes) |
| `build/cmident` | `/media/fat/ConsoleMode/tools/cmident` (what a game file is, for the apps' box art lookups: a cartridge's CRC32 the way No-Intro counts it, a disc's product code, `.chd` too; cmremote runs it only while a lookup asks) |
| `remote/ua/ua_bridge.py`, `remote/ua/status.json` | `/media/fat/ConsoleMode/tools/ua/` (Update All's settings for the apps) |
| `modules/lib`, `modules/update-guard` | `/media/fat/ConsoleMode/tools/modules/` (restore points of MiSTer's Main and Linux) |
| `modules/update-guard.ini` | `/media/fat/ConsoleMode/tools/update-guard.ini` |
| Memory Card Tool core | `/media/fat/_Utility/SS1_MemCard_<date>.rbf` |
| Controller Tester core | `/media/fat/_Utility/SS1_Controller_Tester_<date>.rbf` |

The cores are separate GPL programs, launched, not linked; Console Mode can ship them as they are.
`tools/cmmod.c` calls every function from the command line: it's the reference for wiring each one up.

## Memory card manager

**Actions:** back up the card in front port 1, check it against an image, write an image to it
(`cmod_job_start(CMOD_JOB_MC_BACKUP / _VERIFY / _WRITE, image, ...)`). Only sectors that differ are written,
each one read back. Always back the card up before a write: build the new image from that backup.

**Card contents** (ps1_memcard_lib, in `vendor/`): `mc_parse()` lists the saves (name, product code,
region, title, blocks), `mc_icon()` gives each save's 16x16 icon as RGBA, `mc_export()` / `mc_import()`
copy a save between images as .mcs, `mc_remove()` deletes one, `mc_blank()` makes a formatted card.
**Library:** `mclib_store()` keeps each backup as a version (one folder per card), `mclib_recognise()` tells
which known card was just read (by its saves), `mclib_list()` / `mclib_versions()` / `mclib_load()`.

**After a job** (when the front end starts again): `cmod_job_result()` gives `state` (done/failed),
`message` (for example "Card backed up", "No card in front port 1"), the counts (`ok`, `retried`,
`failed`, `differ`, `written`), `card` (0 none, 1 formatted, 2 not formatted) and `error`.

**Screens to build:**
1. *Memory cards:* the library (cards, their versions), with Back up card, and per card: Restore to card,
   Check card.
2. *Card contents:* the saves of a card or version, with icons and titles; Copy to another card, Delete,
   Export (.mcs), Import (.mcs).
3. *Result:* shown when the front end comes back after a job (the message, the counts). The core shows
   its own progress on the TV while it works (a map of the 1024 sectors), so no progress screen is needed.
4. A confirmation before any write ("The card will be backed up first").

## SNAC controller tester

**Action:** `cmod_job_start(CMOD_JOB_PADTEST, NULL, 0, NULL, 0)`. The tester core shows both front ports
live on the TV (buttons, both sticks, analog mode, a memory card in each port, and a wiring check), so
this module needs no screen of its own. Holding **Select+Start for 2 seconds** on either controller brings
the front end back; closing the core from MiSTer's menu works too.

**Data, for a front end that wants to draw it:** `cmod_pad_parse()` decodes the core's report
(`CMOD_PAD_DIR/cm_report.bin` while it runs): per port present, ID and type (`cmod_pad_type()`), buttons
(`CMOD_PAD_*`, `cmod_pad_button_name()`), LX LY RX RY (0..255), memory card present. That's also what a
menu core with the front ports built in would feed a live screen.

**Screens to build:** a menu entry ("Test front-port controllers") and a one-line hint before it starts
("Hold Select+Start for 2 seconds to come back").

## Drive repair (offline SD card repair)

The SD card can't be repaired while the SuperStation runs from it. `sd_offline_repair.sh` puts a copy of the SuperStation's
own Linux on a USB or NVMe drive (`cmrepair/rescue/os.img`, about 400 MB) and a few lines in
`linux/u-boot.txt` that do nothing until asked. **Arm**, then **restart**: u-boot starts the copy from the
drive, which runs fsck.exfat on the unmounted SD card (and every other drive plugged in), leaves the
result on the drive and restarts normally.

`cmod_repair_run(cmd, arg, out, n)` runs a command and returns its key=value output; `cmod_kv()` picks a key.

| Command | What it does | Keys |
|---|---|---|
| `status` | the repair drive and what's plugged in | `drive`, `drive_present`, `drive_mount`, `drive_model`, `image`, `uboot`, `armed`, `sd_dirty_at_boot`, `candidate=mount\|partuuid\|model\|free` (one per drive that can be used), `plugged=...`, `last_result` (clean / repaired / errors_left / failed), `last_at`, `last_summary` |
| `setup <drive>` | make that drive (`/media/usbN`) the repair drive (copies Linux to it, a few minutes) | |
| `arm` / `disarm` | the next warm restart repairs the SD card / not after all | |
| `restart` | a warm restart now (a cold reset clears the request) | |
| `result` | after the repair's restart: picks the result up from the drive (run it at start-up) | `result` |
| `remove` | the lines off the SD card, the copy off the drive | |

**Screens to build:**
1. *Drive repair:* the repair drive (or "none yet": the candidates to pick from), the last result and when,
   "the SD card wasn't shut down cleanly" when `sd_dirty_at_boot=1`.
2. *Set up:* pick a drive from `candidate`, then a progress/wait screen while `setup` runs.
3. *Repair now:* a confirmation ("The SuperStation restarts, repairs the SD card and restarts again,
   about a minute; the TV stays dark meanwhile"), then `arm` + `restart`.
4. *Result:* at start-up, call `result`; when it returns one, show it (clean / repaired / errors left).

## The app link (remote access for the apps)

`cmremote` lets the phone and PC apps (Console Mode for Windows) see and change Console Mode's game
library over the home network: the systems and games, box art (look at it, send new art), details
(description, year, developer, publisher, genre, players), and start a game.

**It reads Console Mode's own files and keeps no library of its own:**

| What | Where it comes from |
|---|---|
| Systems | `themeconfig/section_groups/*.ini` (romExts, romDirs; each `/media/fat/...` folder also on the USB/NVMe drives) |
| Games | the files in those folders, with Console Mode's one-game-folder rule (a disc set's folder is one game) |
| Box art | `<folder>/media/<title>.png`, else `<system folder>/media/<title>.png`. New art is saved there, 512 px wide, and Console Mode's `media/optimized/` copy is removed so it makes a new one |
| Details | `gamelist.xml` in the system's folder (EmulationStation's format). Console Mode shows them after Settings > Game Library > Import gamelist.xml |
| Favourites, play history | `gamelist.ini`, `playtime/history.tsv`, read only (Console Mode rewrites them itself) |
| Starting a game | an MGL for the systems whose core slot is known (NES, SNES, Genesis, PlayStation, Game Boy, GBA, N64...); .mgl/.mra files as they are |

Not there (Console Mode doesn't keep them): "based on" links for hacks, scraper states, memory card and
controller screens over the network, backups, Wi-Fi. Those commands answer with a short error the apps show.

**Install:** `build/cmremote` to `/media/fat/ConsoleMode/tools/`. Console Mode calls `cmod_remote_start()` when it
starts (cmremote keeps running while games play). It's off until it's turned on. Settings and paired
devices: `tools/remote.ini` (only hashes of the keys); log: `tools/cmremote.log`.

**Screens to build:**
1. *Settings > Remote access:* on/off (`cmod_remote_cmd("on"/"off")`), the address to type in the app
   (`address=` from `info`), the paired devices (`device=` lines) with Forget (`forget <id>`).
2. *Pair a device:* `pair`, then show `code=` big with the seconds left (`code_left=`), polling `info` every
   second; when `paired=` has a name, say "<name> is paired" and close. Back: `pair_stop`.

**The apps:** Console Mode for Windows (cm_win, a private repository) is the PC app for it.

Also: `groups` and `browse` list the library the way Console Mode's Load Game does (groups, systems, then
folders and games), and `GET /font/regular|bold` gives Console Mode's font so an app can look like the TV.

`cmmod remote info|on|off|pair|pair_stop|forget ID|rescan` does each step from a shell. Tested on a
SuperStation One with Console Mode 1.2.5: 1448 games in 3 seconds, pairing, box art, details, the last game
played. `make test` runs the same against a pretend SD card and USB drive.

### Update All and the restore points (for the PC app)

`{"cmd":"ua"}` is Update All's whole settings list, read by Update All's own code on the SuperStation (`ua_bridge.py`): change any item, uninstall what it installed, and `{"cmd":"ua","do":"run"}` runs it. Nothing is
blocked. The one protection is update-guard: a run from the app first saves a restore
point of MiSTer's Main, the menu core, the kernel and Linux; `{"cmd":"guard"}` lists them and rolls back,
and puts back settings inside Linux (SSH keys, the hostname) after a Linux update. `{"cmd":"job"}` is the
output of the run or rollback while it goes.

**An Update All started anywhere is covered**, the TV's Scripts menu too (Console Mode has no wrapper around
it): a few minutes after cmremote starts, the system files as they are are saved unless that exact set is
saved already (checksums; a copy only when something changed them), and while it runs a watcher sees Update All
or the Downloader start and does the same straight away, before they get to MiSTer's Main or Linux. So what
was there before any update is always saved. `cmremote boot on` starts it at boot from MiSTer's
`linux/user-startup.sh` (backed up first), so the app can still roll back when an update broke Console Mode.
