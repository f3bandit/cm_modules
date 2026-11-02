#!/bin/bash
# Console Mode backup-agent: snapshot saves and settings when a game exits, and once a day.
# Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see LICENSE); Retro Remake: see LICENSE-RETRO-REMAKE.md.
#
# Commands
#   run                         watch for game exits (started from user-startup.sh)
#   now                         take a snapshot now (always, even if nothing changed)
#   list                        snapshots, newest first
#   restore <snapshot> [--only saves|savestates|...] [--dry-run] [--force]
#                               put a snapshot back (or only those folders/files from it); takes a
#                               "pre-restore" snapshot first
#   stop                        stop the watcher
#   status                      key=value summary for tools
#
# Settings (optional): /media/fat/ConsoleMode/tools/backup-agent.ini. Started by the app link (cmremote) at boot.

VERSION="0.2.2"

SELF="$(readlink -f "$0" 2>/dev/null || echo "$0")"
HERE="${SELF%/*}"
LIB="$HERE/../lib/common.sh"
[ -f "$LIB" ] || { echo "backup-agent: missing $LIB" >&2; exit 1; }
# shellcheck source=../lib/common.sh
. "$LIB"

TOOLS="${BACKUP_AGENT_TOOLS:-/media/fat/ConsoleMode/tools}"
INI="${BACKUP_AGENT_INI:-$TOOLS/backup-agent.ini}"
STATE="$TOOLS/state/backup-agent.state"
BTMP="${CM_TMP:-/tmp/cmguard}"
mkdir -p "$TOOLS/state" "$BTMP" 2>/dev/null
CORENAME_FILE="${BACKUP_AGENT_CORENAME:-/tmp/CORENAME}"
LOCK="backup-agent"

log_init "$BTMP/backup.log"

# ---------- settings ----------

BASE_PATH="${BACKUP_AGENT_BASE:-/media/fat}"     # the SD card
DEST="$TOOLS/backups/saves"
KEEP="20"
ON_GAME_EXIT="true"
DAILY="true"
SOURCES="saves|savestates|config|MiSTer.ini|MiSTer_*.ini|linux/user-startup.sh|linux/wpa_supplicant.conf|Scripts/*.ini|ConsoleMode/config.ini|ConsoleMode/gamelist.ini|ConsoleMode/playtime|ConsoleMode/tools/remote.ini"
MIN_FREE_MB="200"
EXIT_DELAY_SECONDS="5"
MIN_GAP_SECONDS="120"

load_settings() {
	ini_load "$INI" BASE_PATH DEST KEEP ON_GAME_EXIT DAILY SOURCES MIN_FREE_MB \
		EXIT_DELAY_SECONDS MIN_GAP_SECONDS
	case "$KEEP" in ''|*[!0-9]*) KEEP=20 ;; esac
	[ "$KEEP" -lt 1 ] && KEEP=1
	case "$MIN_FREE_MB" in ''|*[!0-9]*) MIN_FREE_MB=200 ;; esac
	case "$EXIT_DELAY_SECONDS" in ''|*[!0-9]*) EXIT_DELAY_SECONDS=5 ;; esac
	case "$MIN_GAP_SECONDS" in ''|*[!0-9]*) MIN_GAP_SECONDS=120 ;; esac
}

# ---------- what to back up ----------

ITEMS=()   # paths relative to BASE_PATH that exist right now

collect_items() {
	local pat match IFS_SAVE
	ITEMS=()
	IFS_SAVE="$IFS"; IFS='|'
	local pats=()
	read -r -a pats <<< "$SOURCES"
	IFS="$IFS_SAVE"
	for pat in "${pats[@]}"; do
		[ -n "$pat" ] || continue
		case "$pat" in /*|*..*) log "ignoring source '$pat' (must be inside $BASE_PATH)"; continue ;; esac
		for match in "$BASE_PATH"/$pat; do
			[ -e "$match" ] || continue
			ITEMS+=("${match#"$BASE_PATH"/}")
		done
	done
}

# fingerprint : changes whenever a file in the sources is added, removed, resized or rewritten
fingerprint() {
	( cd "$BASE_PATH" && find "${ITEMS[@]}" -type f -exec stat -c '%n|%s|%Y' {} + 2>/dev/null | sort | md5sum ) | cut -c1-32
}

# ---------- snapshots ----------

current_core() {
	local c=""
	[ -f "$CORENAME_FILE" ] && read -r c < "$CORENAME_FILE"
	printf '%s' "$c"
}

dest_ok() {
	local mp
	mp="$(mountpoint_of "$DEST")"
	if [ "$mp" = "/" ]; then
		# "/" is Linux's own file system (linux.img): a missing USB drive or share lands here
		LAST_ERROR="$DEST is not on a mounted drive (drive unplugged or share not mounted?)"
		return 1
	fi
	mkdir -p "$DEST" 2>/dev/null || { LAST_ERROR="can't create $DEST"; return 1; }
	return 0
}

# snapshot <reason> <force 0|1>
snapshot() {
	local reason="$1" force="$2" fp last need free name tmp stamp
	LAST_ERROR=""
	collect_items
	if [ "${#ITEMS[@]}" = 0 ]; then
		log "nothing to back up (no sources found under $BASE_PATH)"
		return 0
	fi
	fp="$(fingerprint)"
	last="$(state_get "$STATE" fingerprint)"
	if [ "$force" != 1 ] && [ "$fp" = "$last" ]; then
		log "no changes since the last snapshot ($reason)"
		return 0
	fi
	dest_ok || { log "snapshot skipped: $LAST_ERROR"; state_set "$STATE" last_error "$LAST_ERROR"; return 1; }

	need="$(cd "$BASE_PATH" && du -sk "${ITEMS[@]}" 2>/dev/null | awk '{ s += $1 } END { print s + 0 }')"
	free="$(free_kb "$DEST")"
	if [ -n "$free" ] && [ $((free - need)) -lt $((MIN_FREE_MB * 1024)) ]; then
		LAST_ERROR="not enough space on $DEST (needs up to $((need / 1024)) MB, keeps ${MIN_FREE_MB} MB free)"
		log "snapshot skipped: $LAST_ERROR"
		state_set "$STATE" last_error "$LAST_ERROR"
		return 1
	fi

	stamp="$(date '+%Y%m%d-%H%M%S')"
	name="snap-$stamp-${reason//[^A-Za-z0-9_.-]/_}.tar.gz"
	local n=2
	while [ -e "$DEST/$name" ]; do      # two snapshots in the same second
		name="snap-$stamp-$n-${reason//[^A-Za-z0-9_.-]/_}.tar.gz"
		n=$((n + 1))
	done
	tmp="$DEST/.partial-$name"
	if ! ( cd "$BASE_PATH" && tar -czf "$tmp" "${ITEMS[@]}" ) 2>"$BTMP/backup.err"; then
		LAST_ERROR="tar failed: $(head -c 200 "$BTMP/backup.err")"
		rm -f "$tmp"
		log "snapshot failed: $LAST_ERROR"
		state_set "$STATE" last_error "$LAST_ERROR"
		return 1
	fi
	if ! tar -tzf "$tmp" >/dev/null 2>&1; then
		LAST_ERROR="the new snapshot didn't read back cleanly"
		rm -f "$tmp"
		log "snapshot failed: $LAST_ERROR"
		state_set "$STATE" last_error "$LAST_ERROR"
		return 1
	fi
	mv -f "$tmp" "$DEST/$name"
	sync
	state_set "$STATE" fingerprint "$fp"
	state_set "$STATE" last_snapshot "$name"
	state_set "$STATE" last_at "$(date +%s)"
	state_set "$STATE" last_error ""
	log "snapshot $name ($(du -k "$DEST/$name" | cut -f1) KB)"
	prune
	return 0
}

snapshots() {
	# newest first; names sort by time
	find "$DEST" -maxdepth 1 -type f -name 'snap-*.tar.gz' 2>/dev/null | sed 's|.*/||' | sort -r
}

prune() {
	local n=0 f
	while read -r f; do
		[ "$f" = "${PROTECT:-}" ] && continue
		n=$((n + 1))
		[ "$n" -le "$KEEP" ] && continue
		rm -f "${DEST:?}/$f" && log "removed old snapshot $f"
	done < <(snapshots)
}

# ---------- watcher ----------

daily_due() {
	local last now
	[ "$DAILY" = "true" ] || return 1
	last="$(state_get "$STATE" last_at)"
	now="$(date +%s)"
	[ -z "$last" ] || [ $((now - last)) -ge 86400 ]
}

recent() {
	local last
	last="$(state_get "$STATE" last_at)"
	[ -n "$last" ] && [ $(( $(date +%s) - last )) -lt "$MIN_GAP_SECONDS" ]
}

# on_core <new core name> : called whenever the running core changes
PREV_CORE=""
on_core() {
	local new="$1" old="$PREV_CORE"
	PREV_CORE="$new"
	[ -n "$old" ] && [ "$old" != "$new" ] || return 0
	log "core: $old -> $new"
	if [ "$new" = "MENU" ] && [ "$ON_GAME_EXIT" = "true" ]; then
		# give the core's last save write time to land
		sleep "$EXIT_DELAY_SECONDS"
		recent && { log "skipping: last snapshot was under ${MIN_GAP_SECONDS}s ago"; return 0; }
		snapshot "exit-$old" 0
	fi
	[ "$new" = "MENU" ] && maybe_daily
	return 0
}

maybe_daily() {
	daily_due || return 0
	[ "$(current_core)" = "MENU" ] || return 0    # not while a game might be writing saves
	snapshot "daily" 0 || return 0
	# a daily run with no changes still counts as done for today
	state_set "$STATE" last_at "$(date +%s)"
}

watch_inotify() {
	local ev file ipid
	# MiSTer ships inotifywait (inotify-tools); busybox inotifyd is the fallback
	if command -v inotifywait >/dev/null 2>&1; then
		exec 4< <(exec inotifywait -q -m -e close_write "$CORENAME_FILE" 2>/dev/null)
	else
		# "-" makes inotifyd print events instead of running a program
		exec 4< <(exec inotifyd - "$CORENAME_FILE:w" 2>/dev/null)
	fi
	ipid=$!
	INOTIFY_PID=$ipid
	# sleeps until a core changes (no timer); the daily backup is checked then too (on_core)
	while read -r ev file <&4; do
		on_core "$(current_core)"
	done
	exec 4<&-
	kill "$ipid" 2>/dev/null
	: "$ev" "$file"
}

watch_poll() {
	local c
	while :; do
		sleep 10 & wait $!
		c="$(current_core)"
		[ "$c" != "$PREV_CORE" ] && on_core "$c"
	done
}

cmd_run() {
	load_settings
	lock_take "$LOCK" || exit 0
	INOTIFY_PID=""
	trap 'lock_drop "$LOCK"; [ -n "$INOTIFY_PID" ] && kill "$INOTIFY_PID" 2>/dev/null' EXIT
	trap 'exit 0' TERM INT
	log "watching $CORENAME_FILE (dest $DEST, keep $KEEP)"

	# Main writes CORENAME once the menu is up; wait for it (and for network shares to settle)
	local i
	for ((i = 0; i < 120; i++)); do
		[ -f "$CORENAME_FILE" ] && break
		sleep 1 & wait $!
	done
	sleep "${BACKUP_AGENT_SETTLE:-30}" & wait $!
	PREV_CORE="$(current_core)"
	# (no backup at start-up: nothing heavy runs unless something happened. The daily one is taken
	# the next time a game is closed.)

	# event-driven when inotify tools exist; otherwise a 10 s check of a file in RAM
	if command -v inotifywait >/dev/null 2>&1 || command -v inotifyd >/dev/null 2>&1; then
		watch_inotify
	fi
	watch_poll
}

# ---------- foreground commands ----------

cmd_now() {
	load_settings
	if snapshot "manual" 1; then echo "Snapshot: $(state_get "$STATE" last_snapshot)"; else echo "Failed: $LAST_ERROR"; return 1; fi
}

cmd_list() {
	load_settings
	local f
	while read -r f; do
		printf '%s\t%s KB\n' "$f" "$(du -k "$DEST/$f" | cut -f1)"
	done < <(snapshots)
}

cmd_restore() {
	local name="" dry=0 force=0 only=""
	while [ $# -gt 0 ]; do
		case "$1" in
			--dry-run) dry=1 ;;
			--force) force=1 ;;
			--only) only="${2:-}"; shift ;;
			*) name="$1" ;;
		esac
		shift
	done
	load_settings
	[ -n "$name" ] || { echo "Usage: backup-agent.sh restore <snapshot> [--only saves|savestates|...] [--dry-run] [--force]"; return 1; }
	name="${name##*/}"
	[ -f "$DEST/$name" ] || { echo "No snapshot $name in $DEST"; return 1; }
	# --only: just these top-level folders/files from the snapshot (e.g. saves), nothing else
	local members=() shown=() want=() o m first
	if [ -n "$only" ]; then
		local IFS_SAVE="$IFS"
		IFS='|'; read -r -a want <<< "$only"; IFS="$IFS_SAVE"
		for o in "${want[@]}"; do
			o="${o%/}"
			# literal names only (a folder or a file), no wildcards
			case "$o" in ''|/*|*..*|*[\*\?\[]*) echo "Only plain names for --only: '$o'"; return 1 ;; esac
			first=""
			while IFS= read -r m; do
				shown+=("$m")
				[ -z "$first" ] && first="$m"
			done < <(tar -tzf "$DEST/$name" 2>/dev/null | P="$o" awk 'index($0, ENVIRON["P"]) == 1 && (length($0) == length(ENVIRON["P"]) || substr($0, length(ENVIRON["P"]) + 1, 1) == "/")')
			# extract the folder (or file) itself: tar takes everything under it
			[ -n "$first" ] && members+=("$first")
		done
		[ "${#members[@]}" -gt 0 ] || { echo "Nothing from '$only' in $name; nothing changed."; return 1; }
	fi
	if [ "$dry" = 1 ]; then
		echo "Would restore into $BASE_PATH:"
		if [ "${#shown[@]}" -gt 0 ]; then printf '%s\n' "${shown[@]}"; else tar -tzf "$DEST/$name"; fi
		return 0
	fi
	if [ "$force" != 1 ] && [ -f "$CORENAME_FILE" ] && [ "$(current_core)" != "MENU" ]; then
		echo "A core is running ($(current_core)); go back to the menu first, or use --force."
		return 1
	fi
	tar -tzf "$DEST/$name" >/dev/null 2>&1 || { echo "Snapshot $name is damaged; nothing changed."; return 1; }
	PROTECT="$name"    # never prune the snapshot we're about to restore
	if ! snapshot "pre-restore" 1; then
		echo "Couldn't take a safety snapshot first ($LAST_ERROR); nothing changed."
		return 1
	fi
	local safety
	safety="$(state_get "$STATE" last_snapshot)"
	if ( cd "$BASE_PATH" && tar -xzf "$DEST/$name" "${members[@]}" ); then
		sync
		state_set "$STATE" fingerprint ""
		log "restored $name (undo: restore $safety)"
		echo "Restored $name."
		echo "To undo: backup-agent.sh restore $safety"
		return 0
	fi
	echo "Restore failed part way; undo with: backup-agent.sh restore $safety"
	return 1
}

cmd_stop() {
	local owner
	owner="$(lock_owner "$LOCK")"
	if [ -n "$owner" ] && kill "$owner" 2>/dev/null; then echo "Stopped."; else echo "Not running."; fi
}

cmd_status() {
	load_settings
	echo "version=$VERSION"
	echo "watcher=$([ -n "$(lock_owner "$LOCK")" ] && echo running || echo stopped)"
	echo "dest=$DEST"
	echo "count=$(snapshots | wc -l)"
	echo "keep=$KEEP"
	echo "last_snapshot=$(state_get "$STATE" last_snapshot)"
	local at
	at="$(state_get "$STATE" last_at)"
	echo "last_at=$([ -n "$at" ] && date -d "@$at" '+%Y-%m-%d %H:%M:%S' 2>/dev/null)"
	echo "last_error=$(state_get "$STATE" last_error)"
	local free d="$DEST"
	while [ ! -d "$d" ] && [ "$d" != "/" ]; do d="$(dirname "$d")"; done   # not made until the first snapshot
	free="$(free_kb "$d")"
	echo "free_mb=$(( ${free:-0} / 1024 ))"
}

case "${1:-}" in
	run)     cmd_run ;;
	now)     cmd_now ;;
	list)    cmd_list ;;
	restore) shift; cmd_restore "$@" ;;
	stop)    cmd_stop ;;
	status)  cmd_status ;;
	version) echo "$VERSION" ;;
	*) sed -n '2,14p' "$SELF" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
