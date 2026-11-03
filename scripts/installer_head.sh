#!/bin/bash
# Setup_Console_Mode_App.sh (@VERSION@): the SuperStation side of Console Mode for Windows and the phone app.
# Run it from Console Mode's Scripts menu. It only writes its own files (ConsoleMode/tools, one Scripts entry,
# a few lines in linux/user-startup.sh), never Console Mode's own, and keeps a copy of the tools it replaces in
# ConsoleMode/tools/backups/tools/<date>; if anything goes wrong on the way, that copy is put back.
# Not Console Mode's own installer (Install_Console_Mode.sh): this one only adds the app's side, and takes itself
# off the Scripts menu once it's done.
# Copyright (C) 2026 f3bandit. GPL-3.0-or-later. Source: github.com/f3bandit/cm_modules
FAT=/media/fat
CM=$FAT/ConsoleMode
T=$CM/tools
VER="@VERSION@"

echo ""
echo "Console Mode tools $VER"
echo "======================="
if [ ! -d "$CM" ] || [ ! -f "$CM/ConsoleMode_arm" ]; then
	echo "Console Mode isn't installed on this SD card: install Console Mode first."
	sleep 10
	exit 1
fi
free_kb=$(df -k "$FAT" | awk 'NR==2 {print $4}')
if [ "${free_kb:-0}" -lt 20000 ]; then
	echo "Not enough room on the SD card (needs 20 MB)."
	sleep 10
	exit 1
fi

TMP=$(mktemp -d /tmp/cmtools.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
LINE=$(awk '/^__CMTOOLS_PAYLOAD__$/ { print NR + 1; exit }' "$0")
if [ -z "$LINE" ] || ! tail -n +"$LINE" "$0" | tar xzf - -C "$TMP" 2>/dev/null; then
	echo "This installer is damaged (download it again). Nothing was changed."
	sleep 10
	exit 1
fi
if ! (cd "$TMP" && md5sum -c MANIFEST.md5 >/dev/null 2>&1); then
	echo "This installer's files don't check out (download it again). Nothing was changed."
	sleep 10
	exit 1
fi

# a copy of what's there now, to put back if this goes wrong (the newest 3 kept)
STAMP=$(date +%Y%m%d-%H%M%S)
BK=$T/backups/tools/$STAMP
mkdir -p "$BK"
had=0
for f in $(cd "$TMP" && find ConsoleMode/tools Scripts -type f 2>/dev/null); do
	if [ -f "$FAT/$f" ]; then mkdir -p "$BK/$(dirname "$f")"; cp -p "$FAT/$f" "$BK/$f"; had=1; fi
done
[ -f "$FAT/linux/user-startup.sh" ] && cp -p "$FAT/linux/user-startup.sh" "$BK/user-startup.sh"
echo "$VER" > "$BK/.installing"
ls -1d "$T"/backups/tools/*/ 2>/dev/null | sort -r | tail -n +4 | xargs -r rm -rf

rollback() {
	echo ""
	echo "Something went wrong: putting the tools back as they were..."
	if [ "$had" = 1 ]; then (cd "$BK" && find ConsoleMode Scripts -type f 2>/dev/null) | while read -r f; do cp -p "$BK/$f" "$FAT/$f.cm-new" && mv -f "$FAT/$f.cm-new" "$FAT/$f"; done; fi
	[ -f "$BK/user-startup.sh" ] && cp -p "$BK/user-startup.sh" "$FAT/linux/user-startup.sh"
	sync
	[ -x "$T/cmremote" ] && ! pidof cmremote >/dev/null 2>&1 && start-stop-daemon -S -b -x "$T/cmremote" -- serve >/dev/null 2>&1
	echo "Back as it was. Nothing else was touched."
	sleep 10
	exit 1
}

echo ""
echo "Installing..."
# settings are kept: only put in when there's none yet
keep() { case "$1" in ConsoleMode/tools/update-guard.ini|ConsoleMode/tools/remote.ini) [ -f "$FAT/$1" ];; *) return 1;; esac; }
P=$(pidof cmremote)
[ -n "$P" ] && kill $P 2>/dev/null
sleep 1
for f in $(cd "$TMP" && find . -type f ! -name MANIFEST.md5 | sed 's|^\./||'); do
	keep "$f" && continue
	mkdir -p "$FAT/$(dirname "$f")" || rollback
	# .new then mv: a running program's file can't be written over
	cp "$TMP/$f" "$FAT/$f.cm-new" && mv -f "$FAT/$f.cm-new" "$FAT/$f" || rollback
done
chmod 755 "$T"/cm* "$T"/*.sh "$FAT/Scripts/Pair_Console_Mode_App.sh" 2>/dev/null
sync

# the app link: started now and at every boot
"$T/cmremote" boot on >/dev/null 2>&1 || rollback
start-stop-daemon -S -b -x "$T/cmremote" -- serve >/dev/null 2>&1
sleep 2
pidof cmremote >/dev/null 2>&1 || rollback
"$T/cmremote" on >/dev/null 2>&1
rm -f "$BK/.installing"
echo "$VER" > "$T/.tools-version"
sync
echo "Done: Console Mode tools $VER."
echo "(The tools they replaced are kept in ConsoleMode/tools/backups/tools/$STAMP.)"
sleep 3

# done with: off the Scripts menu, so only Pair_Console_Mode_App is left there (the app has the next one)
case "$0" in "$FAT/Scripts/"*|"$FAT/scripts/"*) rm -f "$0" ;; esac

# straight on to pairing the PC or phone
exec /bin/bash "$FAT/Scripts/Pair_Console_Mode_App.sh"
