#!/bin/sh
# Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
# Builds dist/SS1ConsoleMode.exe: a windowed program (no cmd window), no debug info. Refuses to finish if the
# result would open a console.
set -e
cd "$(dirname "$0")"
mkdir -p dist
CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -trimpath -ldflags "-H windowsgui -s -w" -o dist/SS1ConsoleMode.exe .
python3 - <<'PY'
import struct, sys
b = open("dist/SS1ConsoleMode.exe", "rb").read(4096)
pe = struct.unpack_from("<I", b, 0x3c)[0]
sub = struct.unpack_from("<H", b, pe + 24 + 68)[0]
if sub != 2:
    sys.exit("dist/SS1ConsoleMode.exe would open a console window (subsystem %d)" % sub)
PY
v=$(sed -n 's/^\tappVersion = "\(.*\)"/\1/p' main.go)
echo "dist/SS1ConsoleMode.exe $v (windowed)"; sha256sum dist/SS1ConsoleMode.exe
