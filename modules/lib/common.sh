#!/bin/bash
# Console Mode modules: shared helpers.
# Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see LICENSE.md in cm_modules).
#
# Sourced by every module. Plain bash + busybox, nothing else assumed.

CM_ROOT="${CM_ROOT:-/media/fat/ConsoleMode/tools/guard}"
CM_TMP="${CM_TMP:-/tmp/cmguard}"
CM_USER_STARTUP="${CM_USER_STARTUP:-/media/fat/linux/user-startup.sh}"
# Installing into an SD card image that's being built (an image build): CM_OFFLINE=1.
# Files, settings and boot entries are written, nothing is started and nothing outside the card is
# touched; CM_TARGET_ROOT is where CM_ROOT will be on the card (boot entries point there).
CM_OFFLINE="${CM_OFFLINE:-0}"
CM_TARGET_ROOT="${CM_TARGET_ROOT:-$CM_ROOT}"
offline() { [ "$CM_OFFLINE" = 1 ]; }

# ---------- logging ----------

# log_init <file> : log to <file>, keep it under ~256 KB
log_init() {
	LOG_FILE="$1"
	mkdir -p "${LOG_FILE%/*}" 2>/dev/null
	if [ -f "$LOG_FILE" ] && [ "$(wc -c < "$LOG_FILE" 2>/dev/null || echo 0)" -gt 262144 ]; then
		tail -n 400 "$LOG_FILE" > "$LOG_FILE.tmp" 2>/dev/null && mv -f "$LOG_FILE.tmp" "$LOG_FILE"
	fi
}

uptime_s() {
	local u
	read -r u _ < /proc/uptime 2>/dev/null || u=0
	printf '%s' "${u%%.*}"
}

log() {
	local line
	line="$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null) [+$(uptime_s)s] $*"
	[ -n "${LOG_FILE:-}" ] && printf '%s\n' "$line" >> "$LOG_FILE" 2>/dev/null
	[ -t 1 ] && printf '%s\n' "$*"
	return 0
}

# ---------- ini ----------

_trim() {
	local s="$1"
	s="${s#"${s%%[![:space:]]*}"}"
	s="${s%"${s##*[![:space:]]}"}"
	printf '%s' "$s"
}

# ini_load <file> <KEY>... : set only the listed keys; quotes stripped; CRLF safe
ini_load() {
	local file="$1" line key val k ok
	shift
	[ -f "$file" ] || return 1
	while IFS= read -r line || [ -n "$line" ]; do
		line="${line%$'\r'}"
		case "$line" in *=*) ;; *) continue ;; esac
		key="$(_trim "${line%%=*}")"
		case "$key" in ''|\#*|\;*) continue ;; esac
		ok=0
		for k in "$@"; do [ "$k" = "$key" ] && { ok=1; break; }; done
		[ "$ok" = 1 ] || continue
		val="$(_trim "${line#*=}")"
		case "$val" in
			\"*) val="${val#\"}"; val="${val%%\"*}" ;;
			\'*) val="${val#\'}"; val="${val%%\'*}" ;;
		esac
		printf -v "$key" '%s' "$val"
	done < "$file"
	return 0
}

# state_get <file> <key> / state_set <file> <key> <value> : tiny key=value store
state_get() {
	[ -f "$1" ] || return 1
	local line
	while IFS= read -r line; do
		[ "${line%%=*}" = "$2" ] && { printf '%s' "${line#*=}"; return 0; }
	done < "$1"
	return 1
}

state_set() {
	local file="$1" key="$2" val="$3" tmp
	mkdir -p "${file%/*}" 2>/dev/null
	tmp="$file.$$"
	{ [ -f "$file" ] && grep -v "^$key=" "$file"; printf '%s=%s\n' "$key" "$val"; } > "$tmp" 2>/dev/null
	mv -f "$tmp" "$file"
}

# ---------- timing ----------

# run_limited <seconds> <cmd>... : run cmd, give up after <seconds> (rc 124).
# Never blocks on a process stuck in the kernel (e.g. a dead CIFS server):
# it stops waiting and leaves it behind instead of hanging the caller.
# Output is discarded unless RL_OUT / RL_ERR name files to write it to.
run_limited() {
	local limit="$1" pid i ticks
	shift
	"$@" </dev/null >"${RL_OUT:-/dev/null}" 2>"${RL_ERR:-/dev/null}" &
	pid=$!
	ticks=$((limit * 5))
	for ((i = 0; i < ticks; i++)); do
		if ! kill -0 "$pid" 2>/dev/null; then
			wait "$pid"
			return $?
		fi
		sleep 0.2
	done
	disown "$pid" 2>/dev/null   # no "Killed" job notice
	kill -9 "$pid" 2>/dev/null
	return 124
}

# tcp_open <host> <port> <seconds> : true if a TCP connection succeeds
tcp_open() {
	run_limited "$3" bash -c "exec 3<>/dev/tcp/$1/$2"
}

# ---------- locking ----------

# lock_take <name> : true if we now hold it. Stale locks (dead pid) are reclaimed.
# The lock is a symlink whose target is the holder's pid: making it is one atomic step, so two takers at the
# same moment can't both get it (the old directory-and-pid-file lock let both in while the pid file was being
# written: two restore points saved at once).
lock_take() {
	local link="$CM_TMP/$1.lock" pid
	mkdir -p "$CM_TMP" 2>/dev/null
	ln -s "$$" "$link" 2>/dev/null && return 0
	[ -d "$link" ] && [ ! -L "$link" ] && { pid="$(cat "$link/pid" 2>/dev/null)"; [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && return 1; rm -rf "$link"; ln -s "$$" "$link" 2>/dev/null; return; }
	pid="$(readlink "$link" 2>/dev/null)"
	[ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && return 1
	rm -f "$link"
	ln -s "$$" "$link" 2>/dev/null
}

lock_owner() {
	local pid
	pid="$(readlink "$CM_TMP/$1.lock" 2>/dev/null)"
	[ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && printf '%s' "$pid"
}

lock_drop() {
	local link="$CM_TMP/$1.lock"
	[ "$(readlink "$link" 2>/dev/null)" = "$$" ] && rm -f "$link"
	return 0
}

# ---------- root filesystem ----------

ROOT_WAS_RO=0
root_rw() {
	ROOT_WAS_RO=0
	if grep -qE '^[^ ]+ / [^ ]+ ro[, ]' /proc/mounts; then
		mount -o remount,rw / || return 1
		ROOT_WAS_RO=1
	fi
	return 0
}

root_restore() {
	sync
	[ "$ROOT_WAS_RO" = 1 ] && mount -o remount,ro / 2>/dev/null
	ROOT_WAS_RO=0
	return 0
}

# ---------- managed blocks in shared files ----------
# A block is the lines between "# <tag>: BEGIN" and "# <tag>: END".

# block_strip <file> <tag> : print file without the block
block_strip() {
	awk -v b="# $2: BEGIN" -v e="# $2: END" '
		$0 == b { skip = 1; next }
		$0 == e { skip = 0; next }
		!skip { print }
	' "$1"
}

# block_has <file> <tag>
block_has() {
	[ -f "$1" ] && grep -qxF "# $2: BEGIN" "$1"
}

# block_set <file> <tag> <body> : replace (or add) the block; only writes on change
block_set() {
	local file="$1" tag="$2" body="$3" tmp="$CM_TMP/block.$$"
	mkdir -p "$CM_TMP"
	{
		[ -f "$file" ] && block_strip "$file" "$tag"
		printf '# %s: BEGIN\n%s\n# %s: END\n' "$tag" "$body" "$tag"
	} > "$tmp"
	if [ -f "$file" ] && cmp -s "$tmp" "$file"; then
		rm -f "$tmp"
		return 0
	fi
	cat "$tmp" > "$file" && rm -f "$tmp"
}

# block_remove <file> <tag>
block_remove() {
	local file="$1" tmp="$CM_TMP/block.$$"
	block_has "$file" "$2" || return 0
	mkdir -p "$CM_TMP"
	block_strip "$file" "$2" > "$tmp" && cat "$tmp" > "$file"
	rm -f "$tmp"
}

# mountpoint_of <path> : the mount point that holds <path> (longest match in /proc/mounts)
mountpoint_of() {
	local p
	p="$(readlink -f "$1" 2>/dev/null || echo "$1")"
	awk -v p="$p" '
		{ m = $2; gsub(/\\040/, " ", m) }
		(p == m || index(p, m == "/" ? "/" : m "/") == 1) && length(m) > length(best) { best = m }
		END { print best }
	' /proc/mounts
}

# free_kb <path> : free space in KB on the file system holding <path>
free_kb() {
	df -kP "$1" 2>/dev/null | awk 'NR == 2 { print $4 }'
}

# startup_block_set <tag> <line> / startup_block_remove <tag> : our entry in user-startup.sh
startup_block_set() {
	if [ ! -e "$CM_USER_STARTUP" ]; then
		if [ -e "${CM_USER_STARTUP%/*}/_user-startup.sh" ]; then
			cp "${CM_USER_STARTUP%/*}/_user-startup.sh" "$CM_USER_STARTUP"
		else
			printf '#!/bin/sh\n\n' > "$CM_USER_STARTUP"
		fi
	fi
	block_set "$CM_USER_STARTUP" "$1" "$2" && chmod +x "$CM_USER_STARTUP"
}

startup_block_remove() {
	[ -f "$CM_USER_STARTUP" ] && block_remove "$CM_USER_STARTUP" "$1"
	return 0
}

# install_files <module> <src dir> : copy a module and the shared lib into CM_ROOT/modules
install_files() {
	local mod="$1" src="$2" dest="$CM_ROOT/modules"
	mkdir -p "$dest/lib" "$dest/$mod" || return 1
	if [ "$src" != "$dest/$mod" ]; then
		cp -f "$src/../lib/common.sh" "$dest/lib/common.sh" || return 1
		cp -f "$src"/*.sh "$dest/$mod/" || return 1
		[ -f "$src/../LICENSE.md" ] && cp -f "$src/../LICENSE.md" "$dest/LICENSE.md"
	fi
	chmod +x "$dest/$mod/"*.sh
}

# remove_files <module> : remove an installed module; drop the lib when no modules are left
remove_files() {
	local dest="$CM_ROOT/modules"
	rm -rf "${dest:?}/$1"
	if [ -z "$(find "$dest" -mindepth 1 -maxdepth 1 -type d ! -name lib 2>/dev/null)" ]; then
		rm -rf "${dest:?}/lib" "${dest:?}/LICENSE.md"
	fi
}

# backup <file> <dir> : timestamped copy, prints the backup path
backup() {
	local src="$1" dir="$2" dst
	[ -f "$src" ] || return 0
	mkdir -p "$dir" || return 1
	dst="$dir/${src##*/}.$(date '+%Y%m%d-%H%M%S')"
	cp -p "$src" "$dst" && printf '%s' "$dst"
}
