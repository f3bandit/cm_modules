# SS1 Link

**A debugging and testing tool for a SuperStation One running Console Mode.** It's for developers and
Claude, not for end users, and it's not what the phone/PC apps use: they talk to the SS1 through the
app link (remote access, port 8767). See "The two links" in the main README. (Phosphor Link, in phosphor_os, is the same tool for
Phosphor OS. Every SS1 Link tool says `[SS1 Link / Console Mode]` first, so with both installed it's
always clear which is which.)

A PC program for reaching a MiSTer through the **SuperStation One dock's USB-C debug port**, or
over SSH. People can use it from a command prompt, and the **Claude desktop app** can use it
directly (as an MCP server) to run tests on the MiSTer, copy files, read the boot log, press buttons in
Console Mode and see its screen.

## What the debug port is

The dock's debug port is a USB-to-serial bridge to the DE10-Nano's console UART (115200 8N1).
Everything the MiSTer prints to its console comes out there: U-Boot, the Linux kernel, MiSTer
Main's own messages, and a root login prompt. It works with no network, while the MiSTer boots,
and when the network is broken, which is why SS1 Link prefers it.

SS1 Link is careful with it:

- **It never types while the MiSTer is booting.** A key press in U-Boot's first seconds stops the
  boot. It waits until the console is quiet, or for the login prompt. If it finds U-Boot already
  stopped at its `=>` prompt, it sends `boot` so the MiSTer carries on.
- **Main's messages can't damage results.** Main prints to the same console at any moment, even
  in the middle of a line. Command output is saved to a file on the MiSTer and fetched as
  numbered, length-checked lines; damaged lines are fetched again, and the whole file is checked
  with md5. Files are sent the same way, in checked pieces.
- **It leaves the console as it found it.** Kernel messages are muted while connected and put
  back afterwards, and it logs out when done.

**It keeps the debug port awake and checks it.** Left closed for a while, the dock's USB serial
bridge can stop answering until the cable is unplugged. So SS1 Link opens the debug port soon
after it starts and holds it for as long as it runs, even when it works over SSH, and every 90
seconds checks that the console still answers. One that doesn't is opened again; a command that gets
no answer checks the console straight away, instead of failing every call after it. While SSH is
down (a reboot) it works over the debug port, and goes back to SSH once that answers again. A
terminal program can't use the debug port while SS1 Link (or the Claude app) is running.

## Install

1. Copy `ss1-link.exe` to a folder of its own, e.g. `C:\ss1-link\`.
2. Connect the dock's USB-C debug port to the PC. Windows installs the USB serial driver itself.
3. Open a command prompt in that folder and check it: `ss1-link status`
4. Optional: copy `ss1-link.ini.example` to `ss1-link.ini` to set the COM port, the
   login or an SSH address.

## Using it with the Claude desktop app

```
ss1-link install-claude
```

This adds SS1 Link to the Claude desktop app's settings (`claude_desktop_config.json`,
backed up first). Quit Claude completely (from the tray too) and open it again. Claude can then:

| Tool | What it does |
|---|---|
| `status` | Connects and reports MiSTer Linux version, kernel, uptime, running core, free space |
| `list_ports` | Serial ports on the PC |
| `run` | Runs a command or script on the MiSTer as root; returns output and exit code |
| `push_file` / `push_text` | Copies a file, or writes text, to the MiSTer |
| `pull_file` / `read_file` | Copies a file from the MiSTer, or returns a text file |
| `serial_log` | Listens to the console (Main and kernel messages) without typing |
| `recent_output` | The last part of everything the console printed |
| `press` | Presses buttons in Console Mode from the PC: up, down, left, right, a/ok, b/back, x, y, l, r, select, start (a virtual keyboard and pad on the SS1, `ss1kbd`, started the first time) |
| `screenshot` | Console Mode's screen as a picture (the Linux framebuffer; black while a core is on screen) |
| `cm_state` | Console Mode's version, whether it runs, the core on screen, `.state`, favourites/history, the update and app logs, ss1jobd's last job |
| `reboot` | Reboots and returns the whole boot log, then logs in again |
| `disconnect` | Closes the connection |

`ss1-link uninstall-claude` removes it again (also with a backup).

## Commands

```
ss1-link ports                      list serial ports
ss1-link status                     connect and show the MiSTer's state
ss1-link run "<command>"            run a command (its exit code is passed back)
ss1-link push <local> <remote> [mode]
ss1-link pull <remote> <local>
ss1-link watch [seconds]            show what the console prints, without typing
ss1-link reboot                     reboot and show the whole boot log
ss1-link term                       interactive console (Ctrl-] quits)
ss1-link selftest                   check the link end to end
```

Everything the console prints is also kept in `ss1-link.log` next to the program.

## Speed

The port runs at 115200 baud, about 11 KB/s raw. Files travel as base64, so expect roughly
8 KB/s: a script is instant, a 1 MB file takes about two minutes. Over SSH a 20 MB file takes about a second on a local network, so `auto` uses SSH whenever the SS1
is on the network.

## Building

`./build.sh` (Go 1.26+; go.mod asks for 1.27.3 and Go fetches it) builds `dist/ss1-link.exe` and `dist/ss1-link-linux`. No cgo.

`test/fake_mister.py` is a pretend MiSTer console on a pseudo-terminal: boot log, login, a real
bash, Main messages dropped in at random (including mid-line), U-Boot stops and reboots. It
reports any key sent during boot. `test/mcptest.py` drives the MCP server through every tool.

## Status

0.1.0: from Phosphor Link 0.4.3 (the link, the watchdog, SSH and the debug port unchanged), with Console
Mode's tools: `press`, `screenshot`, `cm_state`. Tested on a SuperStation One with Console Mode 1.2.5:
`press` moved the cursor, `screenshot` showed the menu, `cm_state` read its state. SSH needs `host=` in
`ss1-link.ini` (the SS1's address).
