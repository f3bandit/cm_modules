#!/bin/bash
# Phosphor update-guard: make MiSTer updates safe to take, and to take back.
# Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see LICENSE in ss1_cm_modules); Retro Remake: see LICENSE-RETRO-REMAKE.md.
#
# System files: Update All replaces the same base files every time (MiSTer, menu.rbf, the kernel,
# linux.img). update-guard keeps a copy of the set you're running, so "rollback" can put back
# exactly what you had before the last update (or an older set).
# Settings: a Linux update replaces linux.img, so changes made inside it (a static IP, SSH keys,
# the hostname) go back to stock. update-guard keeps copies and offers to put them back.
# Nothing is rolled back or restored unless you ask (or set AUTO_RESTORE="true" for settings).
#
# Commands
#   update [args]              back up the system files, then run Update All
#   check                      before updating: space, writable SD, system files and settings saved
#   points                     saved system file sets, newest first
#   save-point [label]         save the current system files now (label: a-z0-9-, e.g. before-update-all)
#   delete-point <point>...    delete restore points
#   keep                       after a Linux update: keep the stock settings (clears the alert)
#   ui-state                   key=value lines for Phosphor's Settings screen
#   rollback [point] [--hold]  go back (default: to before the last update); --hold also stops
#                              Update All updating Linux until "unhold"
#   hold / unhold              stop / allow Linux updates by Update All
#   save                       refresh the saved settings now
#   pending                    settings that went back to stock after an update
#   restore [file...] / undo   put saved settings back / reverse the last restore
#   boot                       called from user-startup.sh
#   status                     key=value summary for tools
#
# Settings (optional): /media/fat/phosphor/config/update-guard.ini

VERSION="0.2.0"

SELF="$(readlink -f "$0" 2>/dev/null || echo "$0")"
HERE="${SELF%/*}"
LIB="$HERE/../lib/common.sh"
[ -f "$LIB" ] || { echo "update-guard: missing $LIB" >&2; exit 1; }
# shellcheck source=../lib/common.sh
. "$LIB"

INI="${UPDATE_GUARD_INI:-$PHOSPHOR_ROOT/config/update-guard.ini}"
SAVED="$PHOSPHOR_ROOT/guard/files"
STATE="$PHOSPHOR_ROOT/state/update-guard.state"
UNDO_DIR="$PHOSPHOR_ROOT/backups/update-guard/last-restore"
HISTORY="$PHOSPHOR_ROOT/logs/update-guard.log"
ROOT="${UPDATE_GUARD_ROOTFS:-}"                   # tests point this at a fake root
VERSION_FILE="$ROOT/MiSTer.version"
SD="${UPDATE_GUARD_SD:-/media/fat}"

log_init "$PHOSPHOR_TMP/update-guard.log"


# shellcheck source=restore-points.sh
. "$HERE/restore-points.sh"

# SSH host private keys are left out on purpose: copies on the SD card would be readable by anyone
# with the card or the MiSTer's network share. Add "/etc/ssh/ssh_host_*" to GUARD to include them.
GUARD="/etc/dhcpcd.conf|/etc/hostname|/etc/hosts|/etc/ssh/sshd_config|/root/.ssh/authorized_keys"
AUTO_RESTORE="false"
UPDATE_FREE_MB="600"

load_settings() {
	ini_load "$INI" GUARD AUTO_RESTORE UPDATE_FREE_MB
	case "$UPDATE_FREE_MB" in ''|*[!0-9]*) UPDATE_FREE_MB=600 ;; esac
	load_point_settings
}

history() {
	mkdir -p "${HISTORY%/*}"
	printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$HISTORY"
	log "$*"
}

linux_version() {
	local v=""
	[ -f "$VERSION_FILE" ] && read -r v < "$VERSION_FILE"
	printf '%s' "${v:-unknown}"
}

# guarded files that exist now (absolute paths as Linux sees them, without ROOT)
current_files() {
	local pats=() p f IFS_SAVE
	IFS_SAVE="$IFS"; IFS='|'; read -r -a pats <<< "$GUARD"; IFS="$IFS_SAVE"
	for p in "${pats[@]}"; do
		[ -n "$p" ] || continue
		case "$p" in /*) ;; *) continue ;; esac
		for f in "$ROOT"$p; do
			[ -f "$f" ] && printf '%s\n' "${f#"$ROOT"}"
		done
	done | sort -u
}

# saved copies (absolute paths as Linux sees them)
saved_files() {
	[ -d "$SAVED" ] || return 0
	(cd "$SAVED" && find . -type f | sed 's|^\.||') | sort
}

# save_copies : copy guarded files to the SD card when they changed
save_copies() {
	local f n=0
	while IFS= read -r f; do
		if ! cmp -s "$ROOT$f" "$SAVED$f" 2>/dev/null; then
			mkdir -p "$(dirname "$SAVED$f")"
			cp -p "$ROOT$f" "$SAVED$f" && n=$((n + 1))
		fi
	done < <(current_files)
	[ "$n" -gt 0 ] && log "saved $n changed file(s)"
	state_set "$STATE" saved_version "$(linux_version)"
	state_set "$STATE" saved_at "$(date '+%Y-%m-%d %H:%M:%S')"
	return 0
}

awaiting() { [ "$(state_get "$STATE" awaiting_restore 2>/dev/null)" = 1 ]; }

# differing_files : saved copies that differ from (or are missing in) the live system
differing_files() {
	local f
	while IFS= read -r f; do
		cmp -s "$SAVED$f" "$ROOT$f" 2>/dev/null || printf '%s\n' "$f"
	done < <(saved_files)
}

# pending_files : after an update, the settings that went back to stock and aren't restored yet.
# (Outside that window a difference just means you edited a file; it gets saved at the next boot.)
pending_files() {
	awaiting && differing_files
	return 0
}

# ---------- commands ----------

cmd_boot() {
	# at boot only the cheap part: notice a Linux update and keep the few settings files' copies
	# current. Restore points are saved when an update is started (save-point, from Phosphor's
	# update scripts), never in the background: copying system files competes with everything else.
	load_settings
	boot_settings
}

boot_settings() {
	local p seen now
	now="$(linux_version)"
	seen="$(state_get "$STATE" linux_seen 2>/dev/null)"
	# (installs from before linux_seen: the version the copies were saved under)
	[ -z "$seen" ] && seen="$(state_get "$STATE" saved_version 2>/dev/null)" && [ -n "$seen" ] && state_set "$STATE" linux_seen "$seen"
	if [ -n "$seen" ] && [ "$seen" = "$now" ]; then
		# same Linux as last boot: keep the saved copies current,
		# unless they still hold settings waiting to be restored after an update
		awaiting || save_copies
		return 0
	fi
	state_set "$STATE" linux_seen "$now"
	if [ -z "$(saved_files)" ]; then
		# first run: nothing to compare yet
		save_copies
		history "first run on Linux $now; saved $(current_files | wc -l) file(s)"
		return 0
	fi
	# another Linux version than last boot: linux.img was replaced
	history "Linux update detected: $(state_get "$STATE" saved_version) -> $now"
	state_set "$STATE" updated_from "$(state_get "$STATE" saved_version)"
	state_set "$STATE" updated_at "$(date '+%Y-%m-%d %H:%M:%S')"
	p="$(differing_files)"
	if [ -z "$p" ]; then
		history "no settings were lost"
		save_copies
	elif [ "$AUTO_RESTORE" = "true" ]; then
		state_set "$STATE" awaiting_restore 1
		history "restoring: $(echo "$p" | tr '\n' ' ')"
		do_restore "" >/dev/null
	else
		state_set "$STATE" awaiting_restore 1
		history "settings that went back to stock (restore with: update-guard.sh restore): $(echo "$p" | tr '\n' ' ')"
		# keep the saved copies as they are until the user decides
	fi
}

cmd_save() {
	load_settings
	if [ -n "$(pending_files)" ] && [ "${1:-}" != "--force" ]; then
		echo "Some saved settings haven't been restored since the last update:"
		pending_files | sed 's/^/  /'
		echo "Restore them (update-guard.sh restore), or replace the saved copies with: update-guard.sh save --force"
		return 1
	fi
	state_set "$STATE" awaiting_restore 0
	save_copies
	echo "Saved."
}

cmd_pending() {
	load_settings
	local p
	p="$(pending_files)"
	if [ -z "$p" ]; then echo "Nothing to restore."; return 0; fi
	echo "$p"
}

# do_restore <one file or empty for all pending>
do_restore() {
	local only="$1" f list
	if [ -z "$only" ] && ! awaiting; then
		echo "Nothing to restore (no Linux update since the copies were saved). To restore one file: update-guard.sh restore <path>"
		return 0
	fi
	if [ -n "$only" ]; then
		[ -f "$SAVED$only" ] || { echo "No saved copy of $only"; return 1; }
		list="$only"
	else
		list="$(pending_files)"
	fi
	[ -n "$list" ] || { echo "Nothing to restore."; return 0; }

	rm -rf "$UNDO_DIR"
	mkdir -p "$UNDO_DIR"
	state_set "$STATE" undo_awaiting "$(state_get "$STATE" awaiting_restore 2>/dev/null)"
	root_rw || { echo "Can't make the Linux file system writable."; return 1; }
	while IFS= read -r f; do
		[ -n "$f" ] || continue
		if [ -f "$ROOT$f" ]; then
			mkdir -p "$(dirname "$UNDO_DIR$f")"
			cp -p "$ROOT$f" "$UNDO_DIR$f"
		else
			# the file didn't exist: undo removes it again
			mkdir -p "$(dirname "$UNDO_DIR$f")"
			: > "$UNDO_DIR$f.absent"
		fi
		mkdir -p "$(dirname "$ROOT$f")"
		cp -p "$SAVED$f" "$ROOT$f" && echo "Restored $f" && history "restored $f"
	done <<< "$list"
	root_restore
	[ -z "$(differing_files)" ] && state_set "$STATE" awaiting_restore 0
	echo "Undo with: update-guard.sh undo"
	echo "Some settings (network, SSH) take effect after a reboot."
}

cmd_restore() {
	load_settings
	[ $# -le 1 ] && { do_restore "${1:-}"; return; }
	local f rc=0
	for f in "$@"; do do_restore "$f" || rc=1; done   # (several: one at a time; undo covers the last)
	return $rc
}

# keep: the settings stay as the Linux update left them; the saved copies follow, the alert goes
cmd_keep() {
	load_settings
	state_set "$STATE" awaiting_restore 0
	save_copies
	history "kept the stock settings after the Linux update"
	echo "Kept the settings as they are now."
}

# ui-state: what Phosphor's Settings screen shows, one fact a line (key=value; points and files repeat)
cmd_ui_state() {
	load_settings
	local f p
	echo "linux=$(linux_version)"
	echo "awaiting=$(awaiting && echo 1 || echo 0)"
	echo "updated_from=$(state_get "$STATE" updated_from 2>/dev/null)"
	echo "updated_at=$(state_get "$STATE" updated_at 2>/dev/null)"
	while IFS= read -r f; do
		[ -n "$f" ] || continue
		local reset=0
		awaiting && ! cmp -s "$SAVED$f" "$ROOT$f" 2>/dev/null && reset=1
		echo "file=$f|$reset"
	done < <( (saved_files; current_files) | sort -u)
	while read -r p; do
		[ -n "$p" ] || continue
		echo "point=$p|$(point_info "$p" label)|$(point_info "$p" created)|$(point_info "$p" linux)|$(point_info "$p" complete)"
	done < <(list_points)
}

cmd_undo() {
	local f
	if [ ! -d "$UNDO_DIR" ] || [ -z "$(find "$UNDO_DIR" -type f)" ]; then
		echo "Nothing to undo."
		return 0
	fi
	root_rw || { echo "Can't make the Linux file system writable."; return 1; }
	while IFS= read -r f; do
		case "$f" in
			*.absent) rm -f "$ROOT${f%.absent}"; echo "Removed ${f%.absent}"; history "undo: removed ${f%.absent}" ;;
			*) cp -p "$UNDO_DIR$f" "$ROOT$f"; echo "Put back $f"; history "undo: put back $f" ;;
		esac
	done < <(cd "$UNDO_DIR" && find . -type f | sed 's|^\.||')
	root_restore
	rm -rf "$UNDO_DIR"
	state_set "$STATE" awaiting_restore "$(state_get "$STATE" undo_awaiting 2>/dev/null)"
}

cmd_check() {
	load_settings
	local free ok=0 p
	free="$(free_kb "$SD")"; free=$(( ${free:-0} / 1024 ))
	if [ "$free" -ge "$UPDATE_FREE_MB" ]; then echo "ok    SD free space: $free MB"
	else echo "FAIL  SD free space: $free MB (an update needs about $UPDATE_FREE_MB MB)"; ok=1; fi
	if awk -v m="$SD" '$2 == m && $4 ~ /^rw(,|$)/ { f = 1 } END { exit !f }' /proc/mounts; then echo "ok    SD card is writable"
	else echo "FAIL  SD card is not writable"; ok=1; fi
	p="$(pending_files)"
	if [ -n "$p" ]; then echo "warn  settings from the last update still not restored: $(echo "$p" | tr '\n' ' ')"
	else
		save_copies
		echo "ok    $(saved_files | wc -l) setting file(s) saved for after the update"
	fi
	if [ -n "$(state_get "$STATE" linux_seen 2>/dev/null)" ]; then echo "ok    update detection ready"
	else echo "warn  update detection not armed yet (reboot once, or run: update-guard.sh boot --now)"; fi
	if point_current; then echo "ok    current system files saved: $(current_point)"
	else echo "FAIL  couldn't save the current system files: $LAST_ERROR"; ok=1; fi
	return $ok
}

cmd_status() {
	load_settings
	echo "version=$VERSION"
	echo "linux=$(linux_version)"
	echo "armed=$([ -n "$(state_get "$STATE" linux_seen 2>/dev/null)" ] && echo yes || echo no)"
	echo "saved_files=$(saved_files | wc -l)"
	echo "saved_at=$(state_get "$STATE" saved_at)"
	echo "pending=$(pending_files | wc -l)"
	echo "last_update_from=$(state_get "$STATE" updated_from)"
	echo "last_update_at=$(state_get "$STATE" updated_at)"
	echo "auto_restore=$AUTO_RESTORE"
	echo "points=$(list_points | wc -l)"
	echo "current_point=$(current_point)"
	echo "rollback_to=$(pick_point)"
	echo "linux_held=$([ "$(state_get "$STATE" hold_set 2>/dev/null)" = 1 ] && echo yes || echo no)"
}

cmd_points() {
	load_settings
	local p
	while read -r p; do
		printf '%s\tLinux %s\t%s file(s)\n' "$p" "$(point_info "$p" linux)" "$(wc -l < "$PDIR/$p/manifest")"
	done < <(list_points)
}

cmd_save_point() {
	load_settings
	local label="${1:-manual}"
	case "$label" in *[!a-z0-9-]*|'') label=manual ;; esac
	if save_point "$label"; then echo "Saved restore point $SAVED_POINT"; else echo "Failed: $LAST_ERROR"; return 1; fi
}

cmd_rollback() {
	local p="" hold=0 a
	for a in "$@"; do
		case "$a" in --hold) hold=1 ;; *) p="$a" ;; esac
	done
	load_settings
	if [ -z "$p" ]; then
		p="$(pick_point)" || { echo "No earlier set of system files to go back to."; return 1; }
	fi
	do_rollback "${p##*/}" "$hold"
}

# update : back up the files the update can replace, then run Update All (or downloader)
cmd_update() {
	local runner="" r
	load_settings
	echo "Backing up the current system files..."
	if ! point_current; then
		echo "Couldn't back up the system files ($LAST_ERROR). Not updating."
		return 1
	fi
	echo "Saved: $(current_point)"
	for r in "${UPDATE_GUARD_RUNNER:-}" "$SD/Scripts/update_all.sh" "$SD/Scripts/downloader.sh" "$SD/Scripts/update.sh"; do
		[ -n "$r" ] && [ -f "$r" ] && { runner="$r"; break; }
	done
	[ -n "$runner" ] || { echo "No Update All or downloader script found in $SD/Scripts."; return 1; }
	echo "Running ${runner##*/}..."
	history "update started with ${runner##*/} (backup: $(current_point))"
	bash "$runner" "$@"
	local rc=$?
	history "update finished (exit $rc)"
	echo
	echo "If anything is wrong after rebooting: update-guard.sh rollback"
	return $rc
}

case "${1:-}" in
	boot)    shift; cmd_boot "$@" ;;
	update)  shift; cmd_update "$@" ;;
	points)  cmd_points ;;
	save-point) shift; cmd_save_point "$@" ;;
	delete-point) shift; load_settings; for p in "$@"; do delete_point "$p"; done ;;
	keep)    cmd_keep ;;
	ui-state) cmd_ui_state ;;
	rollback) shift; cmd_rollback "$@" ;;
	hold)    load_settings; hold_linux ;;
	unhold)  load_settings; unhold_linux ;;
	check)   cmd_check ;;
	save)    shift; cmd_save "$@" ;;
	pending) cmd_pending ;;
	restore) shift; cmd_restore "$@" ;;
	undo)    cmd_undo ;;
	status)  cmd_status ;;
	version) echo "$VERSION" ;;
	*) sed -n '2,29p' "$SELF" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
