#!/bin/bash
# Console Mode sound-level: a volume for the system sounds, apart from the background music.
# Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see LICENSE.md).
#
# Console Mode mixes its background music and system sounds (confirm, back, move) together, with one BGM Volume
# for the music and nothing for the sounds. It plays the system sounds from
# /media/fat/ConsoleMode/Sounds/SystemSounds (picked in Settings > System Sounds), at the level that's in the
# files. So this module sets the level in the files: it keeps each original in tools/backups/sounds and puts a
# copy at the chosen level in its place, with a limiter above 100% so loud settings don't clip. The file names
# stay the same, so Console Mode's settings don't change.
#
# Commands
#   status              key=value: level, and each sound with what was done to it
#   set <percent>       the system sounds at <percent> of their original level (25 to 200; 100 = the originals)
#   refresh             the same level again, after sounds were added or changed (new ones become originals)
#   restore             the originals back, and the level to 100
#
# WAV files only (PCM 8/16/24/32-bit, 32-bit float). MP3 sounds are left as they are and listed as skipped.
# Console Mode picks the new level up when it loads its sounds again (picking a sound in Settings, or a restart).

VERSION="0.1.0"

SELF="$(readlink -f "$0" 2>/dev/null || echo "$0")"
HERE="${SELF%/*}"
LIB="$HERE/../lib/common.sh"
[ -f "$LIB" ] || { echo "sound-level: missing $LIB" >&2; exit 1; }
# shellcheck source=../lib/common.sh
. "$LIB"

TOOLS="${SOUND_LEVEL_TOOLS:-/media/fat/ConsoleMode/tools}"
SFX="${SOUND_LEVEL_DIR:-/media/fat/ConsoleMode/Sounds/SystemSounds}"
ORIG="${SOUND_LEVEL_ORIG:-$TOOLS/backups/sounds}"
STATE="${SOUND_LEVEL_STATE:-$TOOLS/state/sound-level.state}"
CMSFX="${CMSFX:-$TOOLS/cmsfx}"
STMP="${CM_TMP:-/tmp/cmguard}"
mkdir -p "$STMP" 2>/dev/null
log_init "$STMP/sound-level.log"

md5() { md5sum < "$1" 2>/dev/null | cut -c1-32; }
key() { printf 'f_%s' "$(printf '%s' "$1" | md5sum | cut -c1-16)"; }   # a state key for a file name
level() { state_get "$STATE" level || echo 100; }

# apply <percent>: every sound at <percent> of its original
apply() {
	local pct="$1" f name k cur ours done=0 kept=0 skipped="" rc
	[ -d "$SFX" ] || { echo "level=$pct"; echo "sounds=0"; echo "note=no $SFX yet: pick custom system sounds in Console Mode first"; state_set "$STATE" level "$pct"; return 0; }
	[ -x "$CMSFX" ] || { echo "error=missing $CMSFX" >&2; return 1; }
	mkdir -p "$ORIG" || { echo "error=can't make $ORIG" >&2; return 1; }
	for f in "$SFX"/*; do
		[ -f "$f" ] || continue
		name="${f##*/}"
		case "$name" in .*|*.cmsfx-tmp) continue ;; esac
		k="$(key "$name")"
		cur="$(md5 "$f")"
		ours="$(state_get "$STATE" "$k")"
		# a sound this module didn't write (new, or changed by the user) becomes the original
		if [ ! -f "$ORIG/$name" ] || [ "$cur" != "$ours" ]; then
			cp -p "$f" "$ORIG/$name.tmp" && mv -f "$ORIG/$name.tmp" "$ORIG/$name" || { echo "error=can't keep the original of $name" >&2; return 1; }
			log "original kept: $name"
		fi
		case "$(printf '%s' "$name" | tr 'A-Z' 'a-z')" in
		*.wav)
			if [ "$pct" = 100 ]; then
				cp -p "$ORIG/$name" "$f.cmsfx-tmp" && mv -f "$f.cmsfx-tmp" "$f"; rc=$?
			else
				"$CMSFX" gain "$pct" "$ORIG/$name" "$f" 2>>"$LOG_FILE"; rc=$?
			fi
			if [ "$rc" = 0 ]; then
				state_set "$STATE" "$k" "$(md5 "$f")"; done=$((done + 1))
			else
				skipped="$skipped${skipped:+, }$name"
				state_set "$STATE" "$k" "$cur"   # left as it was: still the original
			fi ;;
		*)
			skipped="$skipped${skipped:+, }$name"
			state_set "$STATE" "$k" "$cur" ;;
		esac
	done
	# originals of sounds that are gone
	for f in "$ORIG"/*; do
		[ -f "$f" ] || continue
		name="${f##*/}"
		[ -e "$SFX/$name" ] || { rm -f "$f"; log "removed the original of $name (the sound is gone)"; }
	done
	sync
	state_set "$STATE" level "$pct"
	log "level $pct%: $done sound(s) set${skipped:+, skipped: $skipped}"
	echo "level=$pct"
	echo "sounds=$done"
	[ -n "$skipped" ] && echo "skipped=$skipped"
	return 0
}

cmd="${1:-status}"
case "$cmd" in
status)
	echo "version=$VERSION"
	echo "level=$(level)"
	echo "dir=$SFX"
	n=0
	for f in "$SFX"/*; do
		[ -f "$f" ] || continue
		name="${f##*/}"
		case "$name" in .*|*.cmsfx-tmp) continue ;; esac
		n=$((n + 1))
		case "$(printf '%s' "$name" | tr 'A-Z' 'a-z')" in
		*.wav) if [ -f "$ORIG/$name" ] && [ "$(md5 "$f")" = "$(state_get "$STATE" "$(key "$name")")" ]; then s=set; else s=new; fi ;;
		*) s=skipped ;;
		esac
		echo "sound=$s|$name"
	done
	echo "sounds=$n" ;;
set)
	pct="${2:-}"
	case "$pct" in ''|*[!0-9]*) echo "error=the level is a number from 25 to 200" >&2; exit 1 ;; esac
	[ "$pct" -ge 25 ] && [ "$pct" -le 200 ] || { echo "error=the level is 25 to 200 (percent)" >&2; exit 1; }
	lock_take sound-level || { echo "error=busy" >&2; exit 1; }
	apply "$pct"; rc=$?
	lock_drop sound-level
	exit $rc ;;
refresh)
	lock_take sound-level || { echo "error=busy" >&2; exit 1; }
	apply "$(level)"; rc=$?
	lock_drop sound-level
	exit $rc ;;
restore)
	lock_take sound-level || { echo "error=busy" >&2; exit 1; }
	apply 100; rc=$?
	lock_drop sound-level
	exit $rc ;;
*)
	echo "usage: sound-level.sh status | set <25-200> | refresh | restore" >&2
	exit 1 ;;
esac
