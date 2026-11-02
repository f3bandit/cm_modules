# SS1 Link 0.1.0 (Console Mode)

Lets Claude run commands, copy files, watch the console, press buttons in Console Mode and see its
screen on a SuperStation One. For Phosphor OS, use Phosphor Link (phosphor_os) instead; every tool here
starts its description with `[SS1 Link / Console Mode]`, so the two never get mixed up.

- SSH first when `host=` is set in ss1-link.ini, the dock's USB-C debug port otherwise. The boot log and
  reboots always use the debug port.
- Watchdog: SSH keepalives, Ctrl-C and a check when the console stops answering, the link reopened
  by itself, and an old copy still holding the COM port stopped.
- The program is inside the plugin, so updating is just installing the next package. Settings
  (optional) go in ss1-link.ini next to server/ss1-link.exe.

## Install (Windows)

In Claude Code:

    /plugin marketplace add f3bandit/ss1_cm_modules
    /plugin install ss1-link@ss1-cm-modules

Claude Code uses your GitHub login to read the repository. `/plugin marketplace update ss1-cm-modules`
picks up a new version.
