#!/bin/bash
# update-guard: restore points for MiSTer's own system files.
# Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see LICENSE in cm_modules); Retro Remake: see LICENSE-RETRO-REMAKE.md.
#
# Sourced by update-guard.sh.
#
# Every time the stock MiSTer system files change (an Update All brought a new Linux, MiSTer
# program, menu core or kernel), the first boot with the new set saves a restore point. So there
# is always a point for "exactly what you had before the last update", and older ones before that.
# "rollback" puts a point's files back; whatever it replaces is saved first, so it can be undone.
#
# Points share one store: each distinct file is kept once, however many points use it.
# Linux images are identified by their version (/MiSTer.version), because the running image changes
# on every boot; the other files by their checksum.
# Files Console Mode installs itself (listed in owned.list) are never part of a restore point.

POINTS_DIR="$CM_ROOT/restore-points"
OWNED_LIST="$CM_ROOT/owned.list"
DOWNLOADER_INI="${UPDATE_GUARD_DOWNLOADER_INI:-$SD/downloader.ini}"
MD5_CACHE="$CM_ROOT/state/update-guard.md5cache"

# linux.img (375 MB) is left out by default: add "|linux/linux.img" in update-guard.ini to keep it too
SYSTEM_FILES="MiSTer|menu.rbf|linux/zImage_dtb"
KEEP_POINTS="3"

load_point_settings() {
	ini_load "$INI" SYSTEM_FILES KEEP_POINTS POINTS_DIR
	case "$KEEP_POINTS" in ''|*[!0-9]*) KEEP_POINTS=3 ;; esac
	[ "$KEEP_POINTS" -lt 2 ] && KEEP_POINTS=2   # "current" + "before the last update" at least
	STORE="$POINTS_DIR/store"
	PDIR="$POINTS_DIR/points"
}

# ---------- which files, and what they are ----------

is_owned() {
	[ -f "$OWNED_LIST" ] && grep -qxF "$1" "$OWNED_LIST"
}

# system files present now, relative to the SD root
sys_files() {
	local pats=() p f rel IFS_SAVE
	IFS_SAVE="$IFS"; IFS='|'; read -r -a pats <<< "$SYSTEM_FILES"; IFS="$IFS_SAVE"
	for p in "${pats[@]}"; do
		[ -n "$p" ] || continue
		case "$p" in /*|*..*) continue ;; esac
		for f in "$SD"/$p; do
			[ -f "$f" ] || continue
			rel="${f#"$SD"/}"
			is_owned "$rel" || printf '%s\n' "$rel"
		done
	done | sort -u
}

# file_id <relative path> : what this file "is" — linux-<version> for the image, else its md5
file_id() {
	local f="$1" key sum
	if [ "$f" = "linux/linux.img" ]; then
		printf 'linux-%s' "$(linux_version)"
		return
	fi
	key="$f|$(stat -c '%s|%Y|%Z|%i' "$SD/$f")"
	sum="$(state_get "$MD5_CACHE" "$key" 2>/dev/null)"
	if [ -z "$sum" ]; then
		sum="$(md5sum < "$SD/$f" | cut -c1-32)"
		state_set "$MD5_CACHE" "$key" "$sum"
	fi
	printf '%s' "$sum"
}

# the current set, one "id path" line per file
current_set() {
	local f
	while IFS= read -r f; do
		printf '%s %s\n' "$(file_id "$f")" "$f"
	done < <(sys_files)
}

set_fingerprint() {
	md5sum | cut -c1-32
}

# ---------- points ----------

list_points() {
	[ -d "$PDIR" ] || return 0
	find "$PDIR" -mindepth 1 -maxdepth 1 -type d ! -name '.partial-*' 2>/dev/null | sed 's|.*/||' | sort -r
}

point_info() {
	state_get "$PDIR/$1/info" "$2"
}

points_ok_dest() {
	if [ "$(mountpoint_of "$POINTS_DIR")" = "/" ]; then
		LAST_ERROR="$POINTS_DIR is not on a mounted drive"
		return 1
	fi
	mkdir -p "$STORE" "$PDIR" || { LAST_ERROR="can't create $POINTS_DIR"; return 1; }
}

# copy_image <src> <dst> : copy the running Linux image so the copy is clean
copy_image() {
	local src="$1" dst="$2" was_rw=0 rc
	sync
	# nothing can write to the running image while it's read: read-only for the copy, and back to
	# writable afterwards only if it was writable before (MiSTer normally runs it read-only)
	if [ -z "$ROOT" ] && grep -qE '^[^ ]+ / [^ ]+ rw[, ]' /proc/mounts; then
		mount -o remount,ro / 2>/dev/null && was_rw=1
	fi
	{ nice -n 19 ionice -c 3 cp "$src" "$dst" 2>/dev/null || cp "$src" "$dst"; }
	rc=$?
	[ "$was_rw" = 1 ] && mount -o remount,rw /
	[ "$rc" = 0 ] || { LAST_ERROR="copying ${src##*/} failed"; return 1; }
	if command -v e2fsck >/dev/null 2>&1; then
		e2fsck -fn "$dst" >/dev/null 2>&1 || { LAST_ERROR="the copy of ${src##*/} didn't check out clean"; return 1; }
	fi
	return 0
}

# store_name <id> <relative path>
store_name() {
	printf '%s-%s' "$1" "${2##*/}"
}

# save_point <label> [relative file]... : record the given files (default: all system files)
save_point_unlocked() {
	local label="$1" files=() f id obj need=0 free name tmp
	shift
	if [ "$#" -gt 0 ]; then files=("$@"); else mapfile -t files < <(sys_files); fi
	[ "${#files[@]}" -gt 0 ] || { LAST_ERROR="no system files found under $SD"; return 1; }
	points_ok_dest || return 1

	# only files the store doesn't already have need space
	for f in "${files[@]}"; do
		obj="$STORE/$(store_name "$(file_id "$f")" "$f")"
		[ -f "$obj" ] || need=$((need + $(stat -c %s "$SD/$f") / 1024))
	done
	free="$(free_kb "$POINTS_DIR")"
	if [ "$need" -gt 0 ] && [ -n "$free" ] && [ $((free - need)) -lt 204800 ]; then
		LAST_ERROR="not enough space in $POINTS_DIR (needs $((need / 1024)) MB plus 200 MB spare)"
		return 1
	fi

	# names sort by time; the 2-digit counter keeps two points in the same second in order
	local stamp n=1
	stamp="$(date '+%Y%m%d-%H%M%S')"
	while ls -d "$PDIR/$stamp-$(printf '%02d' "$n")-"* >/dev/null 2>&1; do n=$((n + 1)); done
	name="$stamp-$(printf '%02d' "$n")-$label"
	tmp="$PDIR/.partial-$name"
	rm -rf "$tmp"; mkdir -p "$tmp"
	for f in "${files[@]}"; do
		id="$(file_id "$f")"
		obj="$STORE/$(store_name "$id" "$f")"
		if [ ! -f "$obj" ]; then
			log "storing $f ($id)"
			if [ "$f" = "linux/linux.img" ]; then
				copy_image "$SD/$f" "$obj.part" || { rm -f "$obj.part"; rm -rf "$tmp"; return 1; }
			else
				cp -p "$SD/$f" "$obj.part" || { rm -f "$obj.part"; rm -rf "$tmp"; LAST_ERROR="copying $f failed"; return 1; }
				if [ "$(md5sum < "$obj.part" | cut -c1-32)" != "$id" ]; then
					rm -f "$obj.part"; rm -rf "$tmp"; LAST_ERROR="$f changed while it was being copied"; return 1
				fi
			fi
			mv -f "$obj.part" "$obj"
		fi
		printf '%s %s\n' "$id" "$f" >> "$tmp/manifest"
	done
	{
		echo "label=$label"
		echo "created=$(date '+%Y-%m-%d %H:%M:%S')"
		echo "linux=$(linux_version)"
		echo "fingerprint=$(set_fingerprint < "$tmp/manifest")"
		echo "complete=$([ "$#" -gt 0 ] && echo no || echo yes)"
	} > "$tmp/info"
	sync
	mv "$tmp" "$PDIR/$name"
	SAVED_POINT="$name"
	history "restore point saved: $name (Linux $(linux_version))"
	prune_points
	return 0
}

# keep KEEP_POINTS full points and the newest pre-rollback point; drop unused store files
prune_points() {
	local p n=0 r=0 obj used
	while read -r p; do
		[ "$p" = "${PROTECT_POINT:-}" ] && continue    # the point a rollback is reading from
		case "$p" in
			*-pre-rollback) r=$((r + 1)); [ "$r" -le 1 ] && continue ;;
			*) n=$((n + 1)); [ "$n" -le "$KEEP_POINTS" ] && continue ;;
		esac
		rm -rf "${PDIR:?}/$p" && log "removed old restore point $p"
	done < <(list_points)
	used="$(cat "$PDIR"/*/manifest 2>/dev/null | while read -r id f; do store_name "$id" "$f"; echo; done | sort -u)"
	for obj in "$STORE"/*; do
		[ -f "$obj" ] || continue
		case "$obj" in *.part) continue ;; esac
		grep -qxF "${obj##*/}" <<< "$used" || { rm -f "$obj"; log "freed ${obj##*/}"; }
	done
}

newest_full_point() {
	local p
	while read -r p; do
		[ "$(point_info "$p" complete)" = yes ] && { echo "$p"; return 0; }
	done < <(list_points)
	return 1
}

# point_with <fingerprint> : the newest full point holding exactly this set
point_with() {
	local p
	while read -r p; do
		[ "$(point_info "$p" complete)" = yes ] && [ "$(point_info "$p" fingerprint)" = "$1" ] && { echo "$p"; return 0; }
	done < <(list_points)
	return 1
}

# current_point : the point holding what's installed now
current_point() {
	point_with "$(current_set | set_fingerprint)"
}

# point_current : save a point if the system files changed since the newest full point
# one save at a time: an Update All run from the app saves one, and the app link's watcher notices that same run
points_lock() { local i=0; until lock_take restore-points; do sleep 2; i=$((i + 1)); [ "$i" -gt 900 ] && return 1; done; return 0; }

save_point() {
	points_lock || { LAST_ERROR="another restore point is still being saved"; return 1; }
	save_point_unlocked "$@"
	local rc=$?
	lock_drop restore-points
	return $rc
}

point_current() {
	local fp newest rc=0
	points_lock || { LAST_ERROR="another restore point is still being saved"; return 1; }
	fp="$(current_set | set_fingerprint)"
	newest="$(point_with "$fp")"
	# this exact set is already saved (e.g. after a rollback, or by the save that just finished)
	if [ -z "$newest" ]; then
		save_point_unlocked "system" || { history "couldn't save a restore point: $LAST_ERROR"; rc=1; }
	fi
	lock_drop restore-points
	return $rc
}

# delete_point <name> : remove a point and any stored copies no point uses any more
delete_point() {
	local p="${1##*/}"
	[ -n "$p" ] && [ -d "$PDIR/$p" ] || { echo "No restore point $p"; return 1; }
	rm -rf "${PDIR:?}/$p" && echo "Deleted $p" && history "deleted restore point $p"
	KEEP_POINTS=1000 prune_points
}

# ---------- rollback ----------

# the point to go back to by default: the newest full point older than the one matching what's
# installed (so a second rollback keeps going back), or the newest different one if none matches
pick_point() {
	local p fp here="" passed=0
	fp="$(current_set | set_fingerprint)"
	here="$(point_with "$fp")"
	while read -r p; do
		[ "$(point_info "$p" complete)" = yes ] || continue
		if [ -n "$here" ] && [ "$passed" = 0 ]; then
			[ "$p" = "$here" ] && passed=1
			continue
		fi
		[ "$(point_info "$p" fingerprint)" != "$fp" ] && { echo "$p"; return 0; }
	done < <(list_points)
	return 1
}

# do_rollback <point> <hold 0|1>
# fetch_linux <version e.g. 260907> <dest> : that MiSTer Linux release's linux.img, from MiSTer-devel's
# SD installer repository (the same releases MiSTer's own updater installs), checked by 7-Zip's CRC
fetch_linux() {
	local v="$1" dst="$2" rel url w part n=0 ca=()
	rel="release_20$v"
	w="$POINTS_DIR/download"
	rm -rf "$w"; mkdir -p "$w" || return 1
	[ -f /etc/ssl/certs/cacert.pem ] && ca=(--cacert /etc/ssl/certs/cacert.pem)
	url="https://raw.githubusercontent.com/MiSTer-devel/SD-Installer-Win64_MiSTer/master"
	echo "Downloading MiSTer Linux $v from MiSTer-devel..."
	if curl -fsL "${ca[@]}" -o "$w/$rel.7z" "$url/$rel.7z" 2>/dev/null; then
		n=1
	else
		rm -f "$w/$rel.7z"
		# newer releases are split in parts
		for part in 001 002 003 004; do
			curl -fsL "${ca[@]}" -o "$w/$rel.7z.$part" "$url/$rel.7z.$part" 2>/dev/null || { rm -f "$w/$rel.7z.$part"; break; }
			n=$((n + 1))
		done
		[ "$n" -gt 0 ] && cat "$w/$rel.7z."* > "$w/$rel.7z" && rm -f "$w/$rel.7z."*
	fi
	[ "$n" -gt 0 ] && [ -s "$w/$rel.7z" ] || { LAST_ERROR="couldn't download $rel (network, or no such release)"; rm -rf "$w"; return 1; }
	echo "Checking it..."
	7zr t "$w/$rel.7z" files/linux/linux.img >/dev/null 2>&1 || { LAST_ERROR="the download is damaged"; rm -rf "$w"; return 1; }
	7zr e -y -o"$w" "$w/$rel.7z" files/linux/linux.img >/dev/null 2>&1 && [ -s "$w/linux.img" ] \
		|| { LAST_ERROR="couldn't unpack linux.img"; rm -rf "$w"; return 1; }
	mv -f "$w/linux.img" "$dst" && sync
	local rc=$?
	rm -rf "$w"
	return $rc
}

do_rollback() {
	local p="$1" hold="$2" id f obj changed=() linux=0
	[ -f "$PDIR/$p/manifest" ] || { echo "No restore point $p"; return 1; }
	PROTECT_POINT="$p"

	echo "Checking restore point $p..."
	while read -r id f; do
		obj="$STORE/$(store_name "$id" "$f")"
		[ -f "$obj" ] || { echo "Restore point is missing its copy of $f; nothing changed."; return 1; }
		if [ "$f" != "linux/linux.img" ] && [ "$(md5sum < "$obj" | cut -c1-32)" != "$id" ]; then
			echo "Restore point copy of $f is damaged; nothing changed."
			return 1
		fi
		if [ ! -f "$SD/$f" ] || [ "$(file_id "$f")" != "$id" ]; then
			changed+=("$f")
		fi
	done < "$PDIR/$p/manifest"

	# Linux itself: points keep only its version; if it differs, that release is downloaded
	local want_linux need_linux=0
	want_linux="$(point_info "$p" linux)"
	if ! grep -q ' linux/linux.img$' "$PDIR/$p/manifest" && [ -n "$want_linux" ] && [ "$want_linux" != unknown ] \
		&& [ "$want_linux" != "$(linux_version)" ]; then
		need_linux=1
	fi

	if [ "${#changed[@]}" = 0 ] && [ "$need_linux" = 0 ]; then
		echo "Your system files already match $p."
		return 0
	fi
	if [ "$need_linux" = 1 ]; then
		fetch_linux "$want_linux" "$SD/linux/linux.img.new" || { echo "Linux $want_linux: $LAST_ERROR; nothing changed."; return 1; }
	fi
	echo "Will put back: ${changed[*]}"

	# record what's there now, so this can be undone
	local existing=()
	for f in "${changed[@]}"; do [ -f "$SD/$f" ] && existing+=("$f"); done
	SAVED_POINT=""
	if [ "${#existing[@]}" -gt 0 ] && ! save_point "pre-rollback" "${existing[@]}"; then
		echo "Couldn't save the current files first ($LAST_ERROR); nothing changed."
		return 1
	fi
	local undo="$SAVED_POINT"

	# the same swap MiSTer's own updater uses: write beside, then rename over; Linux image last
	while read -r id f; do
		printf '%s\n' "${changed[@]}" | grep -qxF "$f" || continue
		[ "$f" = "linux/linux.img" ] && { linux=1; continue; }
		obj="$STORE/$(store_name "$id" "$f")"
		{ cp -p "$obj" "$SD/$f.new" && sync && mv -f "$SD/$f.new" "$SD/$f"; } \
			|| { echo "Failed on $f. Undo with: update-guard.sh rollback $undo"; return 1; }
		echo "Put back $f"
	done < "$PDIR/$p/manifest"
	if [ "$need_linux" = 1 ]; then
		mv -f "$SD/linux/linux.img.new" "$SD/linux/linux.img" \
			|| { echo "Failed on linux/linux.img. Undo with: update-guard.sh rollback $undo"; return 1; }
		echo "Put back linux/linux.img (Linux $want_linux, downloaded)"
		linux=1
	elif [ "$linux" = 1 ]; then
		id="$(awk '$2 == "linux/linux.img" { print $1 }' "$PDIR/$p/manifest")"
		{ cp "$STORE/$(store_name "$id" linux/linux.img)" "$SD/linux/linux.img.new" && sync && \
			mv -f "$SD/linux/linux.img.new" "$SD/linux/linux.img"; } \
			|| { echo "Failed on linux/linux.img. Undo with: update-guard.sh rollback $undo"; return 1; }
		echo "Put back linux/linux.img (Linux ${id#linux-})"
	fi
	sync
	history "rolled back to $p (Linux $(point_info "$p" linux)); undo point: ${undo:-none}"

	[ "$hold" = 1 ] && [ "$linux" = 1 ] && hold_linux
	echo
	echo "Done. Reboot to start the restored system."
	[ -n "$undo" ] && echo "To undo: update-guard.sh rollback $undo"
	if [ "$hold" != 1 ]; then
		echo "Note: the next Update All will update these files again. Add --hold to keep Linux at this version."
	fi
	return 0
}

# ---------- holding Linux updates (downloader.ini [mister] update_linux) ----------
# (section names are compared ignoring case, as the Downloader reads them: [MiSTer] is [mister])

ini_value() {
	# ini_value <file> <section> <key>
	awk -v s="[$2]" -v k="$3" '
		{ line = $0; sub(/\r$/, "", line) }
		line ~ /^\[/ { in_s = (tolower(line) == tolower(s)); next }
		in_s { split(line, kv, "="); key = kv[1]; gsub(/[ \t]/, "", key)
		       if (key == k) { v = substr(line, index(line, "=") + 1); gsub(/^[ \t]+|[ \t]+$/, "", v); print v; exit } }
	' "$1" 2>/dev/null
}

# ini_put <file> <section> <key> <value|""> : set, add, or (empty value) remove a key
ini_put() {
	local file="$1" tmp="$CM_TMP/ini.$$"
	mkdir -p "$CM_TMP"
	[ -f "$file" ] || : > "$file"
	awk -v s="[$2]" -v k="$3" -v v="$4" '
		function emit() { if (!done && v != "") print k " = " v; done = 1 }
		{ line = $0; sub(/\r$/, "", line) }
		line ~ /^\[/ { if (in_s) emit(); in_s = (tolower(line) == tolower(s)); if (in_s) seen = 1; print; next }
		in_s { split(line, kv, "="); key = kv[1]; gsub(/[ \t]/, "", key)
		       if (key == k) { emit(); next } }
		{ print }
		END { if (in_s) emit(); else if (!seen && v != "") { print ""; print s; print k " = " v } }
	' "$file" > "$tmp" && cat "$tmp" > "$file"
	rm -f "$tmp"
}

hold_linux() {
	local before
	if [ "$(state_get "$STATE" hold_set 2>/dev/null)" = 1 ]; then echo "Linux updates are already held."; return 0; fi
	before="$(ini_value "$DOWNLOADER_INI" mister update_linux)"
	backup "$DOWNLOADER_INI" "${UPDATE_GUARD_BACKUPS:-$CM_ROOT/backups/update-guard}" >/dev/null
	ini_put "$DOWNLOADER_INI" mister update_linux false
	state_set "$STATE" hold_set 1
	state_set "$STATE" hold_before "${before:-<absent>}"
	history "Linux updates held (downloader.ini update_linux = false)"
	echo "Linux updates are held: Update All won't update Linux until you run: update-guard.sh unhold"
}

unhold_linux() {
	local before
	if [ "$(state_get "$STATE" hold_set 2>/dev/null)" != 1 ]; then echo "Linux updates aren't held by update-guard."; return 0; fi
	before="$(state_get "$STATE" hold_before)"
	backup "$DOWNLOADER_INI" "${UPDATE_GUARD_BACKUPS:-$CM_ROOT/backups/update-guard}" >/dev/null
	if [ "$before" = "<absent>" ]; then
		ini_put "$DOWNLOADER_INI" mister update_linux ""
	else
		ini_put "$DOWNLOADER_INI" mister update_linux "$before"
	fi
	state_set "$STATE" hold_set 0
	history "Linux updates released"
	echo "Linux updates released; the next Update All can update Linux again."
}
