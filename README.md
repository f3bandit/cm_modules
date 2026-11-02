# ss1_cm_modules

The SuperStation One's front-port and drive tools as modules for a front end such as Console Mode: the
**memory card manager**, the **SNAC controller tester** and the **offline drive repair**. Everything except
the screens: each module gives the front end data and actions, and the front end draws them.

The controller tester and the memory card manager are f3bandit's idea and design (first in
[SS1 Tool](https://github.com/f3bandit/ss1_tool), then [Phosphor](https://github.com/f3bandit/phosphor_os)).

**Licence:** GPL-3.0-or-later (LICENSE). **Retro Remake and Taki Udon** have their own permission on top
(LICENSE-RETRO-REMAKE.md): use any of it in Console Mode or any product of theirs, closed source included,
without the GPL's conditions; the only condition is crediting f3bandit.

## Which is for which

| Folder | What it is | Who uses it |
|---|---|---|
| `include/`, `src/` | The modules: memory card manager, controller tester, drive repair (`ss1mod.h`) | Console Mode, linked in |
| `tools/ss1jobd.c` | Runs a memory card / controller job while Console Mode is closed | installed on the SS1 |
| `tools/ss1mod.c` | Every module function from the command line (reference and testing) | developers, on the SS1 |
| `vendor/` | ps1_memcard_lib: card images, saves, icons, the card library | the memory card module |
| `scripts/` | The offline SD card repair | the drive repair module |
| `remote/` | **The app link** (ss1remote): the phone and PC apps reach the game library over the home network | Console Mode, installed on the SS1 |
| `ss1-link/` | **SS1 Link, for debugging and testing only**: lets Claude (or a command prompt) reach an SS1 running Console Mode over SSH or the dock's debug port, press buttons, see the screen. Not part of Console Mode, and not what the apps use (see below) | developers, on a PC |

## The two links: one for the apps, one for debugging

There are two different "links" to an SS1. They don't overlap:

| | **App link** (remote access) | **SS1 Link** (`ss1-link/`) |
|---|---|---|
| What for | The **apps**: the phone and PC apps manage the game library (games, box art, details) over the home network | **Debugging and testing**: a developer (or Claude) runs commands, copies files, reads the boot log, presses buttons and takes screenshots |
| Who uses it | End users, through an app | Developers only, never end users |
| Runs on | A service on the SS1 that answers the apps | A PC program (plus the Claude plugin) |
| Connection | HTTP, port 8767, home network only, the app pairs once with a 6-digit code from the TV | SSH (root) or the dock's USB-C debug port (serial console) |
| Ships with Console Mode | Yes | No. It's a tool for working on Console Mode, not a part of it |
| In Phosphor | phosphord's remote access ([remote-api.md](https://github.com/f3bandit/phosphor_os/blob/main/docs/remote-api.md)) | Phosphor Link (`tools/phosphor-link`) |
| Here | `remote/` (ss1remote) | `ss1-link/` |

SS1 Link's tools all say `[SS1 Link / Console Mode]` first. With Phosphor Link also installed in Claude,
that makes it clear which one talks to which system.

- C11, one header (`include/ss1mod.h`), usable from C++. No dependencies beyond libc and POSIX.
- `make test` runs the host tests; `make arm` builds `ss1jobd` and `ss1mod` for the SS1 (static armhf).
- Tested on a SuperStation One running Console Mode 1.2.5: backup, verify and write of a real card (Console Mode
  closed and came back by itself each time), the controller tester (a DualShock in port 1, Select+Start back),
  and the repair status with an existing repair drive.

## How it fits together

While one of the front-port cores runs, the front end doesn't: the core has the screen. So a job goes to
**`ss1jobd`**, a small program that outlives the front end:

```
front end                         ss1jobd                                   core
---------                         -------                                   ----
ss1_job_start(BACKUP, path)  -->  writes the job + card files and an MGL
(MiSTer closes the front end)     load_core <mgl>  ---------------------->  reads the card, progress
                                  follows the job file                 <--  "SMCP" ... "SMCR" report
                                  copies the backup to `path`
                                  load_core menu.rbf  (front end back)
front end starts again       <--  SS1_RESULT: state=done, message=..., counts
ss1_job_result("message")
```

Install (what Console Mode's installer would do):

| File | Where |
|---|---|
| `build/ss1jobd` | `/media/fat/ConsoleMode/ss1/ss1jobd` |
| `scripts/sd_offline_repair.sh`, `scripts/sd_offline_repair_boot.sh` | `/media/fat/ConsoleMode/ss1/` |
| `build/ss1remote` | `/media/fat/ConsoleMode/ss1/ss1remote` |
| [SS1 Memory Card Tool core](https://github.com/f3bandit/SS1_MemCard_MiSTer) | `/media/fat/_Utility/SS1_MemCard_<date>.rbf` |
| [SS1 Controller Tester core](https://github.com/f3bandit/SS1_Controller_Tester_MiSTer) | `/media/fat/_Utility/SS1_Controller_Tester_<date>.rbf` |

The cores are separate GPL programs, launched, not linked; Console Mode can ship them as they are.
`tools/ss1mod.c` calls every function from the command line: it's the reference for wiring each one up.

## Memory card manager

**Actions:** back up the card in front port 1, check it against an image, write an image to it
(`ss1_job_start(SS1_JOB_MC_BACKUP / _VERIFY / _WRITE, image, ...)`). Only sectors that differ are written,
each one read back. Always back the card up before a write: build the new image from that backup.

**Card contents** (ps1_memcard_lib, in `vendor/`): `mc_parse()` lists the saves (name, product code,
region, title, blocks), `mc_icon()` gives each save's 16x16 icon as RGBA, `mc_export()` / `mc_import()`
copy a save between images as .mcs, `mc_remove()` deletes one, `mc_blank()` makes a formatted card.
**Library:** `mclib_store()` keeps each backup as a version (one folder per card), `mclib_recognise()` tells
which known card was just read (by its saves), `mclib_list()` / `mclib_versions()` / `mclib_load()`.

**After a job** (when the front end starts again): `ss1_job_result()` gives `state` (done/failed),
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

**Action:** `ss1_job_start(SS1_JOB_PADTEST, NULL, 0, NULL, 0)`. The tester core shows both front ports
live on the TV (buttons, both sticks, analog mode, a memory card in each port, and a wiring check), so
this module needs no screen of its own. Holding **Select+Start for 2 seconds** on either controller brings
the front end back; closing the core from MiSTer's menu works too.

**Data, for a front end that wants to draw it:** `ss1_pad_parse()` decodes the core's report
(`SS1_PAD_DIR/cm_report.bin` while it runs): per port present, ID and type (`ss1_pad_type()`), buttons
(`SS1_PAD_*`, `ss1_pad_button_name()`), LX LY RX RY (0..255), memory card present. That's also what a
menu core with the front ports built in would feed a live screen (as Phosphor's does).

**Screens to build:** a menu entry ("Test front-port controllers") and a one-line hint before it starts
("Hold Select+Start for 2 seconds to come back").

## Drive repair (offline SD card repair)

The SD card can't be repaired while the SS1 runs from it. `sd_offline_repair.sh` puts a copy of the SS1's
own Linux on a USB or NVMe drive (`ss1tool/rescue/os.img`, about 400 MB) and a few lines in
`linux/u-boot.txt` that do nothing until asked. **Arm**, then **restart**: u-boot starts the copy from the
drive, which runs fsck.exfat on the unmounted SD card (and every other drive plugged in), leaves the
result on the drive and restarts normally. The same scripts as SS1 Tool's, sharing its state, so SS1 Tool
and the front end see the same repair drive.

`ss1_repair_run(cmd, arg, out, n)` runs a command and returns its key=value output; `ss1_kv()` picks a key.

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

`ss1remote` lets the phone and PC apps (Phosphor for Android and Windows) see and change Console Mode's game
library over the home network: the systems and games, box art (look at it, send new art), details
(description, year, developer, publisher, genre, players), and start a game. It answers the same requests as
Phosphor's remote access, so the same apps work ([remote-api.md](https://github.com/f3bandit/phosphor_os/blob/main/docs/remote-api.md)).

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

**Install:** `build/ss1remote` to `/media/fat/ConsoleMode/ss1/`. Console Mode calls `ss1_remote_start()` when it
starts (ss1remote keeps running while games play). It's off until it's turned on. Settings and paired
devices: `ss1/remote.ini` (only hashes of the keys); log: `ss1/ss1remote.log`.

**Screens to build:**
1. *Settings > Remote access:* on/off (`ss1_remote_cmd("on"/"off")`), the address to type in the app
   (`address=` from `info`), the paired devices (`device=` lines) with Forget (`forget <id>`).
2. *Pair a device:* `pair`, then show `code=` big with the seconds left (`code_left=`), polling `info` every
   second; when `paired=` has a name, say "<name> is paired" and close. Back: `pair_stop`.

**The apps:** Phosphor for Windows and Android look for `"name":"Phosphor"` in `/info` today. Until they also
accept Console Mode, `name = Phosphor` in `remote.ini` makes them connect (`/info` also says
`"frontend":"Console Mode"`).

`ss1mod remote info|on|off|pair|pair_stop|forget ID|rescan` does each step from a shell. Tested on a
SuperStation One with Console Mode 1.2.5: 1448 games in 3 seconds, pairing, box art, details, the last game
played. `make test` runs the same against a pretend SD card and USB drive.
