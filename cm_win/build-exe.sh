#!/bin/sh
# Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
# Builds dist/cm_win.exe: a windowed program (no cmd window), no debug info. Refuses to finish if the
# result would open a console.
set -e
cd "$(dirname "$0")"
mkdir -p dist
# the exe's icon, version and manifest (needs mingw's windres; the .syso in git is used without it)
if command -v x86_64-w64-mingw32-windres >/dev/null; then
	v=$(sed -n 's/^\tappVersion = "\(.*\)"/\1/p' main.go)
	sed -i "s/VALUE \"FileVersion\", \".*\"/VALUE \"FileVersion\", \"$v\"/; s/VALUE \"ProductVersion\", \".*\"/VALUE \"ProductVersion\", \"$v\"/" winres/cm_win.rc
	(cd winres && x86_64-w64-mingw32-windres -O coff -i cm_win.rc -o ../rsrc_windows_amd64.syso)
fi
CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -trimpath -ldflags "-H windowsgui -s -w" -o dist/cm_win.exe .
python3 - <<'PY'
import struct, sys
b = open("dist/cm_win.exe", "rb").read(4096)
pe = struct.unpack_from("<I", b, 0x3c)[0]
sub = struct.unpack_from("<H", b, pe + 24 + 68)[0]
if sub != 2:
    sys.exit("dist/cm_win.exe would open a console window (subsystem %d)" % sub)
PY
v=$(sed -n 's/^\tappVersion = "\(.*\)"/\1/p' main.go)
echo "dist/cm_win.exe $v (windowed)"; sha256sum dist/cm_win.exe
