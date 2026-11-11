#!/bin/bash
# Console Mode gamedb: libretro's game lists (No-Intro and Redump), kept on the SuperStation for official game names.
# Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see LICENSE.md in cm_modules).
#
# One copy of the lists for everything that names games: Super MemCard (smcd: each save's official name, and the
# cover its icon is made from), the memory manager, the app link. The lists are libretro-database's (used under its
# terms), downloaded only when asked for, into GAMEDB_DIR as "<list name>.dat".
#
# Commands
#   status                      key=value: dir, lists (how many), then list=<name>|<date>|<bytes> for each
#   update [<list name>...]     download these lists (all the consoles' when none is named) when missing or older
#                               than 30 days; `update --force ...` downloads them whatever their age. Prints
#                               ok/failed/fresh=<name> for each, then done=<how many downloaded>
#   lists                       the list names it knows, one a line
#
# Needs the internet for update; everything else works offline. Two updates never run at once.

VERSION="0.1.0"

SELF="$(readlink -f "$0" 2>/dev/null || echo "$0")"
HERE="${SELF%/*}"
LIB="$HERE/../lib/common.sh"
[ -f "$LIB" ] || { echo "gamedb: missing $LIB" >&2; exit 1; }
# shellcheck source=../lib/common.sh
. "$LIB"

DIR="${GAMEDB_DIR:-/media/fat/ConsoleMode/gamedb}"
LR="${GAMEDB_URL:-https://raw.githubusercontent.com/libretro/libretro-database/master/metadat}"
MAX_AGE_DAYS=30
STMP="${CM_TMP:-/tmp/cmguard}"
mkdir -p "$STMP" 2>/dev/null
log_init "$STMP/gamedb.log"

# the consoles' lists, and which libretro-database folder each is in
NOINTRO=(
	"Nintendo - Nintendo Entertainment System" "Nintendo - Family Computer Disk System"
	"Nintendo - Super Nintendo Entertainment System" "Nintendo - Satellaview" "Nintendo - Nintendo 64"
	"Nintendo - Game Boy" "Nintendo - Game Boy Color" "Nintendo - Game Boy Advance"
	"Sega - Master System - Mark III" "Sega - Game Gear" "Sega - SG-1000" "Sega - Mega Drive - Genesis"
	"NEC - PC Engine - TurboGrafx 16" "NEC - PC Engine SuperGrafx"
)
REDUMP=("Sony - PlayStation" "Sega - Saturn" "Sega - Mega-CD - Sega CD")

folder_of() {   # <list name> : no-intro, redump, or nothing when it isn't one of ours
	local n
	for n in "${NOINTRO[@]}"; do [ "$n" = "$1" ] && { echo no-intro; return; }; done
	for n in "${REDUMP[@]}"; do [ "$n" = "$1" ] && { echo redump; return; }; done
}

# MiSTer's curl has no certificate bundle of its own: Console Mode's, the MiSTer downloader's, else Linux's (2021)
CA=()
for f in /media/fat/ConsoleMode/cacert.pem /media/fat/Scripts/.config/downloader/cacert.pem /etc/ssl/certs/cacert.pem; do
	[ -f "$f" ] && { CA=(--cacert "$f"); break; }
done

fresh() {   # <file> : younger than MAX_AGE_DAYS
	local now t
	[ -s "$1" ] || return 1
	now="$(date +%s)"; t="$(date -r "$1" +%s 2>/dev/null || echo 0)"
	[ $((now - t)) -lt $((MAX_AGE_DAYS * 86400)) ]
}

cmd_update() {
	local force=0 n f url got=0
	[ "$1" = --force ] && { force=1; shift; }
	local names=("$@")
	[ ${#names[@]} = 0 ] && names=("${NOINTRO[@]}" "${REDUMP[@]}")
	lock_take gamedb || { echo "busy=1"; echo "done=0"; return 1; }
	mkdir -p "$DIR"
	for n in "${names[@]}"; do
		f="$(folder_of "$n")"
		[ -n "$f" ] || { echo "unknown=$n"; continue; }
		if [ $force = 0 ] && fresh "$DIR/$n.dat"; then echo "fresh=$n"; continue; fi
		url="$LR/$f/$(printf '%s' "$n" | sed 's/ /%20/g').dat"
		if curl -fsSL "${CA[@]}" --retry 2 --max-time 120 -o "$DIR/$n.dat.part" "$url" && [ -s "$DIR/$n.dat.part" ]; then
			mv -f "$DIR/$n.dat.part" "$DIR/$n.dat"
			echo "ok=$n"; got=$((got + 1)); log "updated: $n"
		else
			rm -f "$DIR/$n.dat.part"
			echo "failed=$n"; log "couldn't download: $n"
		fi
	done
	lock_drop gamedb
	echo "done=$got"
}

cmd_status() {
	local f n=0
	echo "dir=$DIR"
	for f in "$DIR"/*.dat; do [ -f "$f" ] && n=$((n + 1)); done
	echo "lists=$n"
	for f in "$DIR"/*.dat; do
		[ -f "$f" ] || continue
		echo "list=$(basename "$f" .dat)|$(date -r "$f" '+%Y-%m-%d' 2>/dev/null)|$(wc -c < "$f")"
	done
}

case "$1" in
	status) cmd_status ;;
	update) shift; cmd_update "$@" ;;
	lists) printf '%s\n' "${NOINTRO[@]}" "${REDUMP[@]}" ;;
	*) echo "usage: gamedb.sh status | update [--force] [<list name>...] | lists" >&2; exit 2 ;;
esac
