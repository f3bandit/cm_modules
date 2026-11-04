#!/usr/bin/env python3
# Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../../LICENSE.md); Retro Remake: see ../../LICENSE-RETRO-REMAKE.md.
"""Fake MiSTer serial console on a pty, for testing CM Link.
Boot log (no typing allowed), login, a real bash, Main noise injected at random moments
(including mid-line), and reboot."""
import os, pty, sys, time, threading, random, subprocess, select, tty

NOISE = float(os.environ.get("NOISE", "0.15"))
master, slave = pty.openpty()
tty.setraw(slave)
name = os.ttyname(slave)
open(sys.argv[1], "w").write(name)
wlock = threading.Lock()
state = {"phase": "boot", "violations": 0, "reboot": False}
tmp = os.path.dirname(os.path.abspath(sys.argv[1]))
bindir = os.path.join(tmp, "fakebin")
os.makedirs(bindir, exist_ok=True)
flag = os.path.join(tmp, "reboot.flag")
open(os.path.join(bindir, "reboot"), "w").write(f"#!/bin/sh\ntouch {flag}\nsleep 30\n")
open(os.path.join(bindir, "dmesg"), "w").write("#!/bin/sh\nexit 0\n")
for f in ("reboot", "dmesg"):
    os.chmod(os.path.join(bindir, f), 0o755)

deaf = os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])), "deaf.flag")  # a wedged USB bridge: nothing in or out

def w(b):
    if isinstance(b, str):
        b = b.encode()
    if os.path.exists(deaf):
        return
    with wlock:
        os.write(master, b)

def log(*a):
    print("[fake]", *a, file=sys.stderr, flush=True)

BOOT = ["U-Boot 2017.03 (MiSTer)", "DRAM:  1 GiB", "Hit any key to stop autoboot:  1 \x08\x08\x080",
        "Starting kernel ...", "", "Booting Linux on physical CPU 0x0", "Linux version 5.15.1-MiSTer",
        "Freeing unused kernel memory: 1024K", "Run /sbin/init as init process"]

def boot():
    state["phase"] = "boot"
    for i, l in enumerate(BOOT):
        w(l + "\r\n")
        time.sleep(0.25)
        if i == 2 and os.environ.get("UBOOT_STOP") and not state.get("stopped"):
            state["stopped"] = True
            state["phase"] = "uboot"
            w("=> ")
            while readline().strip() != "boot":
                w("=> ")
            log("u-boot: boot command received")
            state["phase"] = "boot"
    w("\r\nWelcome to MiSTer!\r\n")
    state["phase"] = "login"
    w("\r\nMiSTer login: ")

def noise():
    msgs = ["Main: Mounting /media/usb0\n", "video: 1920x1080@60\n", "input: new device js0\n",
            "osd: menu\n", "\x1b[1;32mcore loaded\x1b[0m\n", "spi_uio_cmd(0x23)\n"]
    while True:
        time.sleep(random.uniform(0.05, 0.6))
        if state["phase"] == "shell" and random.random() < NOISE:
            w(random.choice(msgs).replace("\n", "\r\n"))

def readline():
    buf = b""
    while True:
        b = os.read(master, 1)
        if state["phase"] == "boot":
            state["violations"] += 1
            log("VIOLATION: input during boot", b)
        if b in (b"\r", b"\n"):
            w("\r\n")
            return buf.decode(errors="replace")
        buf += b
        if state["phase"] != "password":
            w(b)

def shell():
    im, isl = pty.openpty()
    env = dict(os.environ, PATH=bindir + ":" + os.environ["PATH"], PS1="/root# ", TERM="vt100")
    p = subprocess.Popen(["bash", "--norc", "-i"], stdin=isl, stdout=isl, stderr=isl, env=env,
                         start_new_session=True, cwd="/tmp",
                         preexec_fn=lambda: __import__("fcntl").ioctl(0, __import__("termios").TIOCSCTTY, 0))
    os.close(isl)
    state["phase"] = "shell"
    while True:
        if os.path.exists(flag):
            os.remove(flag)
            p.kill()
            os.close(im)
            log("reboot")
            time.sleep(0.5)
            return "reboot"
        r, _, _ = select.select([master, im], [], [], 0.1)
        if master in r:
            d = os.read(master, 4096)
            if not os.path.exists(deaf):
                os.write(im, d)
        if im in r:
            try:
                d = os.read(im, 4096)
            except OSError:
                d = b""
            if not d:
                p.wait()
                return "exit"
            mid = float(os.environ.get("NOISE_MID", "0"))
            if len(d) > 20 and random.random() < mid:
                k = random.randrange(1, len(d) - 1)
                d = d[:k] + b"Main: spi_uio_cmd(0x23) mid-line noise\r\n" + d[k:]
                state["mid"] = state.get("mid", 0) + 1
            w(d)
        if p.poll() is not None:
            # drain
            try:
                while select.select([im], [], [], 0.05)[0]:
                    d = os.read(im, 4096)
                    if not d: break
                    w(d)
            except OSError:
                pass
            os.close(im)
            return "exit"

threading.Thread(target=noise, daemon=True).start()
time.sleep(float(os.environ.get("BOOT_DELAY", "0")))
boot()
while True:
    u = readline().strip()
    if not u:
        w("\r\nMiSTer login: ")
        continue
    state["phase"] = "password"
    w("Password: ")
    pw = readline().strip()
    if u == "root" and pw == "1":
        log("login ok")
        how = shell()
        log("shell ended:", how, "mid-noise:", state.get("mid", 0), "violations:", state["violations"])
        if how == "reboot":
            boot()
            continue
        state["phase"] = "login"
        w("\r\nMiSTer login: ")
    else:
        time.sleep(0.5)
        state["phase"] = "login"
        w("\r\nLogin incorrect\r\nMiSTer login: ")
