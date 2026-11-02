# CM Link 0.1.0 (Console Mode)

Lets Claude run commands, copy files, watch the console, press buttons in Console Mode and see its
screen on a SuperStation One. Every tool here starts its description with `[CM Link / Console Mode]`.

- SSH first when `host=` is set in cm-link.ini, the dock's USB-C debug port otherwise. The boot log and
  reboots always use the debug port.
- Watchdog: SSH keepalives, Ctrl-C and a check when the console stops answering, the link reopened
  by itself, and an old copy still holding the COM port stopped.
- The program is inside the plugin, so updating is just installing the next package. Settings
  (optional) go in cm-link.ini next to server/cm-link.exe.

## Install (Windows)

In Claude Code:

    /plugin marketplace add f3bandit/cm_modules
    /plugin install cm-link@cm-modules

Claude Code uses your GitHub login to read the repository. `/plugin marketplace update cm-modules`
picks up a new version.
