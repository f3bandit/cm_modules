#!/bin/bash
# update-guard: make MiSTer updates safe to take, and to take back.
# Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see LICENSE.md in cm_modules).
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
#   current                    save the system files as they are unless that exact set is saved already
#                              (Console Mode's app link: after boot and when an Update All starts)
#   delete-point <point>...    delete restore points
#   keep                       after a Linux update: keep the stock settings (clears the alert)
#   ui-state                   key=value lines for the app's Updates screen
#   rollback [point] [--hold]  go back (default: to before the last update); --hold also stops
#                              Update All updating Linux until "unhold"
#   hold / unhold              stop / allow Linux updates by Update All
#   guard on|off               the Linux guard: on (the default) keeps Linux updates held at every start-up
#                              and before every run; off is the user's choice, after a warning
#   save                       refresh the saved settings now
#   pending                    settings that went back to stock after an update
#   restore [file...] / undo   put saved settings back / reverse the last restore
#   boot                       called from user-startup.sh
#   cm-install <version>       install a Console Mode release (Retro-Remake/ConsoleMode_Distribution): a
#                              restore point and a copy of Console Mode's program files first; it reboots
#   status                     key=value summary for tools
#
# Settings (optional): /media/fat/ConsoleMode/tools/update-guard.ini

VERSION="0.2.0"

SELF="$(readlink -f "$0" 2>/dev/null || echo "$0")"
HERE="${SELF%/*}"
LIB="$HERE/../lib/common.sh"
[ -f "$LIB" ] || { echo "update-guard: missing $LIB" >&2; exit 1; }
# shellcheck source=../lib/common.sh
. "$LIB"

INI="${UPDATE_GUARD_INI:-$CM_ROOT/config/update-guard.ini}"
SAVED="${UPDATE_GUARD_SAVED:-$CM_ROOT/guard/files}"
STATE="$CM_ROOT/state/update-guard.state"
UNDO_DIR="${UPDATE_GUARD_UNDO:-$CM_ROOT/backups/update-guard/last-restore}"
HISTORY="$CM_ROOT/logs/update-guard.log"
ROOT="${UPDATE_GUARD_ROOTFS:-}"                   # tests point this at a fake root
VERSION_FILE="$ROOT/MiSTer.version"
SD="${UPDATE_GUARD_SD:-/media/fat}"

log_init "$CM_TMP/update-guard.log"


# shellcheck source=restore-points.sh
. "$HERE/restore-points.sh"

# SSH host private keys are left out on purpose: copies on the SD card would be readable by anyone
# with the card or the MiSTer's network share. Add "/etc/ssh/ssh_host_*" to GUARD to include them.
GUARD="/etc/dhcpcd.conf|/etc/hostname|/etc/hosts|/etc/ssh/sshd_config|/root/.ssh/authorized_keys"
AUTO_RESTORE="false"
UPDATE_FREE_MB="600"

load_settings() {
	ini_load "$INI" GUARD AUTO_RESTORE UPDATE_FREE_MB LINUX_GUARD
	case "$LINUX_GUARD" in off|false|no|0) LINUX_GUARD=off ;; *) LINUX_GUARD=on ;; esac
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
	# current. Restore points are saved when an update is started (save-point, from the app's
	# update scripts), never in the background: copying system files competes with everything else.
	load_settings
	boot_settings
	guard_linux
}

# The Linux guard (on unless the user turned it off): Update All doesn't update MiSTer Linux (downloader.ini
# [mister] update_linux = false). Put back at every start-up and before every run from the app, so a setting
# changed elsewhere (Update All's own menu) doesn't slip through; turned off only by the user, warned first.
guard_linux() {
	[ "$LINUX_GUARD" = off ] && return 0
	[ "$(ini_value "$DOWNLOADER_INI" mister update_linux)" = false ] && [ "$(state_get "$STATE" hold_set 2>/dev/null)" = 1 ] && return 0
	state_set "$STATE" hold_set 0
	hold_linux >/dev/null
}

# guard on|off: the switch (kept in update-guard.ini), applied now
cmd_guard() {
	local want="$1" tmp
	case "$want" in on|off) ;; *) echo "guard on|off"; return 1 ;; esac
	mkdir -p "${INI%/*}"
	[ -f "$INI" ] || : > "$INI"
	tmp="$INI.new"
	grep -v '^[[:space:]]*LINUX_GUARD[[:space:]]*=' "$INI" > "$tmp"
	echo "LINUX_GUARD=\"$want\"" >> "$tmp"
	mv "$tmp" "$INI"
	load_settings
	if [ "$want" = on ]; then guard_linux; echo "Linux guard on: Update All won't update MiSTer Linux."
	else unhold_linux >/dev/null; history "Linux guard turned off by the user (warned)"; echo "Linux guard off: Update All may update MiSTer Linux."; fi
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

# ui-state: what the app's Updates screen shows, one fact a line (key=value; points and files repeat)
cmd_ui_state() {
	load_settings
	local f p
	echo "linux=$(linux_version)"
	echo "kernel=$(uname -r 2>/dev/null)"
	echo "linux_guard=$LINUX_GUARD"
	echo "linux_held=$([ "$(ini_value "$DOWNLOADER_INI" mister update_linux)" = false ] && echo 1 || echo 0)"
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

# current : the running set saved, unless it is already (cheap then: checksums, no copies). Console Mode has no
# wrapper around Update All on the TV, so the app link keeps the set it runs on saved: whatever starts an update
# next, what was there before it is already safe.
cmd_current() {
	load_settings
	if point_current; then echo "Saved: $(current_point)"; else echo "Failed: $LAST_ERROR"; return 1; fi
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
	guard_linux
	[ "$LINUX_GUARD" = on ] && echo "Linux guard on: MiSTer Linux isn't updated (update_linux = false)."
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

# cm-install <version>: Console Mode's own installer for that release, the way its page says to run it (it
# replaces Console Mode's program files, keeps the settings, then reboots). First: a restore point (MiSTer's
# Main, the menu core, Linux) and a copy of Console Mode's program files (backups/console-mode/<the version
# now>, the last 3 kept), so either can be put back.
cmd_cm_install() {
	local v="${1:-}" now="${2:-unknown}" url w f ca=() bk keep
	case "$v" in ''|*[!0-9.]*) echo "Which Console Mode release? (a version like 1.2.6)"; return 2 ;; esac
	case "$now" in *[!0-9A-Za-z._-]*) now=unknown ;; esac
	load_settings
	echo "Console Mode $v: saving a restore point first..."
	save_point "before-console-mode-${v//./-}" || echo "  (no restore point: $LAST_ERROR; going on: Console Mode's own files are copied next)"
	bk="${UPDATE_GUARD_BACKUPS:-$CM_ROOT/backups/update-guard}"
	bk="${bk%/update-guard}/console-mode/$now"
	mkdir -p "$bk" || { echo "Couldn't make $bk. Not installing."; return 1; }
	for f in ConsoleMode_arm MiSTer_ConsoleMode menu_ConsoleMode.rbf config.ini; do
		[ -f "$SD/ConsoleMode/$f" ] && cp -p "$SD/ConsoleMode/$f" "$bk/$f"
	done
	sync
	echo "Console Mode $now's program files copied to $bk"
	# the last 3 copies only
	keep=0
	for f in $(ls -1td "${bk%/*}"/*/ 2>/dev/null); do keep=$((keep + 1)); [ $keep -gt 3 ] && rm -rf "$f"; done
	url="https://github.com/Retro-Remake/ConsoleMode_Distribution/releases/download/$v/Install_Console_Mode.sh"
	w="$(mktemp -d /tmp/cmupd.XXXXXX)" || return 1
	[ -f "$SD/Scripts/.config/downloader/cacert.pem" ] && ca=(--cacert "$SD/Scripts/.config/downloader/cacert.pem")
	[ ${#ca[@]} = 0 ] && [ -f /etc/ssl/certs/cacert.pem ] && ca=(--cacert /etc/ssl/certs/cacert.pem)
	echo "Downloading Console Mode $v's installer..."
	if ! curl -fsSL "${ca[@]}" --retry 2 -o "$w/Install_Console_Mode.sh" "$url"; then
		echo "Couldn't download it ($url). Nothing was changed."
		rm -rf "$w"; return 1
	fi
	# it has to be Console Mode's installer: its first lines and the package after its marker
	if ! head -c 200 "$w/Install_Console_Mode.sh" | grep -q "Console Mode installer" || ! grep -aq '^__CONSOLEMODE_PAYLOAD__$' "$w/Install_Console_Mode.sh"; then
		echo "That download isn't Console Mode's installer. Nothing was changed."
		rm -rf "$w"; return 1
	fi
	cp "$w/Install_Console_Mode.sh" "$SD/Scripts/Install_Console_Mode.sh.new" && mv -f "$SD/Scripts/Install_Console_Mode.sh.new" "$SD/Scripts/Install_Console_Mode.sh"
	rm -rf "$w"
	sync
	echo "Installing Console Mode $v (the SuperStation restarts when it's done)..."
	echo
	bash "$SD/Scripts/Install_Console_Mode.sh" </dev/null
}

case "${1:-}" in
	boot)    shift; cmd_boot "$@" ;;
	cm-install) shift; cmd_cm_install "$@" ;;
	update)  shift; cmd_update "$@" ;;
	points)  cmd_points ;;
	save-point) shift; cmd_save_point "$@" ;;
	current) cmd_current ;;
	delete-point) shift; load_settings; for p in "$@"; do delete_point "$p"; done ;;
	keep)    cmd_keep ;;
	ui-state) cmd_ui_state ;;
	rollback) shift; cmd_rollback "$@" ;;
	hold)    load_settings; hold_linux ;;
	guard)   shift; load_settings; cmd_guard "$@" ;;
	unhold)  load_settings; unhold_linux ;;
	check)   cmd_check ;;
	save)    shift; cmd_save "$@" ;;
	pending) cmd_pending ;;
	restore) shift; cmd_restore "$@" ;;
	undo)    cmd_undo ;;
	status)  cmd_status ;;
	version) echo "$VERSION" ;;
	*) sed -n '2,31p' "$SELF" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
