#!/bin/sh
# Builds build/Setup_Console_Mode_App.sh: one file a user copies to the SD card's Scripts folder and runs from
# Console Mode's Scripts menu. It puts the SuperStation side of the apps in place (the app link, the job runner,
# the disc, card, drive and update tools) next to Console Mode, never in Console Mode's own files, and shows the
# pairing code. Needs `make arm` first. Optional: CORES=<folder> with the SS1_*.rbf cores to put in _Utility.
# Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE).
set -eu
cd "$(dirname "$0")/.."
VER=${VER:-$(git describe --tags --always 2>/dev/null || date +%Y%m%d)}
S=$(mktemp -d)
trap 'rm -rf "$S"' EXIT
P=$S/payload
mkdir -p "$P/ConsoleMode/tools/ua" "$P/ConsoleMode/tools/modules" "$P/Scripts"
for b in cmremote cmjobd cmmod cmnvme cmtrimfs cmident cmdisc; do
	[ -f "build/$b" ] || { echo "build/$b is missing: make arm first" >&2; exit 1; }
	cp "build/$b" "$P/ConsoleMode/tools/$b"
done
cp scripts/cmtrim.sh scripts/sd_offline_repair.sh scripts/sd_offline_repair_boot.sh "$P/ConsoleMode/tools/"
cp remote/ua/ua_bridge.py remote/ua/status.json "$P/ConsoleMode/tools/ua/"
cp -r modules/lib modules/update-guard modules/backup-agent modules/diag "$P/ConsoleMode/tools/modules/"
cp modules/update-guard.ini "$P/ConsoleMode/tools/update-guard.ini"
cp scripts/Pair_Console_Mode_App.sh "$P/Scripts/Pair_Console_Mode_App.sh"
if [ -n "${CORES:-}" ]; then mkdir -p "$P/_Utility"; cp "$CORES"/SS1_*.rbf "$P/_Utility/"; fi
chmod 755 "$P"/ConsoleMode/tools/cm* "$P"/ConsoleMode/tools/*.sh "$P/Scripts/Pair_Console_Mode_App.sh"
(cd "$P" && find . -type f | sed 's|^\./||' | sort > "$S/files" && xargs md5sum < "$S/files" > MANIFEST.md5)
mkdir -p build
O=build/Setup_Console_Mode_App.sh
sed "s/@VERSION@/$VER/" scripts/installer_head.sh > "$O"
printf '__CMTOOLS_PAYLOAD__\n' >> "$O"
(cd "$P" && tar czf - .) >> "$O"
chmod 755 "$O"
echo "$O ($VER, $(wc -c < "$O") bytes)"
