#!/bin/bash
# Console Mode diag: check the whole MiSTer, hardware and software, and say what's wrong in plain words.
# Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see LICENSE.md).
#
# Commands
#   diag.sh [area...]       run checks (all areas if none given) and print the results
#   diag.sh --kv [area...]  the same as key=level|message lines, for the app link and tools
#   diag.sh report          all checks into a readable file in /media/fat/ConsoleMode/tools/logs/
#   diag.sh bundle          report + logs + settings (passwords removed) in one archive for a bug report
#   diag.sh input           live view of controller and keyboard events (Ctrl-C to stop)
#   diag.sh speed [path]    read/write speed test of the drive holding path (default: the SD card)
#   diag.sh edid <bus>      dump the TV's EDID from the HDMI chip (read only)
#
# Areas: board cores os storage video sound input network usb consolemode
# Levels: ok, info, warn (worth a look), fail (something is broken).
# Everything is read-only except "speed", which writes and removes one temporary file.

VERSION="0.1.3"

SELF="$(readlink -f "$0" 2>/dev/null || echo "$0")"
HERE="${SELF%/*}"
LIB="$HERE/../lib/common.sh"
[ -f "$LIB" ] || { echo "diag: missing $LIB" >&2; exit 1; }
# shellcheck source=../lib/common.sh
. "$LIB"

FAT="${DIAG_FAT:-/media/fat}"
SYS="${DIAG_SYS:-/sys}"
PROC="${DIAG_PROC:-/proc}"
INI="$FAT/MiSTer.ini"
AREAS="board cores os storage video sound input network usb consolemode"
TOOLS="${DIAG_TOOLS:-$FAT/ConsoleMode/tools}"
LOGS="$TOOLS/logs"
DTMP="${CM_TMP:-/tmp/cmguard}"

KV=0
RESULTS=()
FAILS=0
WARNS=0

# res <level> <key> <message>
res() {
	RESULTS+=("$2=$1|$3")
	case "$1" in fail) FAILS=$((FAILS + 1)) ;; warn) WARNS=$((WARNS + 1)) ;; esac
	if [ "$KV" = 1 ]; then
		printf '%s=%s|%s\n' "$2" "$1" "$3"
	else
		printf '%-5s %-26s %s\n' "$1" "$2" "$3"
	fi
}

section() {
	[ "$KV" = 1 ] && return
	printf '\n== %s\n' "$1"
}

ini_get() {
	# ini_get <key> : value from MiSTer.ini's [MiSTer] section (first match)
	[ -f "$INI" ] || return 1
	awk -v k="$1" '
		{ sub(/\r$/, "") }
		/^\[/ { sec = tolower($0); next }
		(sec == "" || sec == "[mister]") && $0 !~ /^[;#]/ {
			split($0, kv, "="); key = kv[1]; gsub(/[ \t]/, "", key)
			if (tolower(key) == tolower(k)) { v = substr($0, index($0, "=") + 1); sub(/^[ \t]+/, "", v); sub(/[ \t;].*$/, "", v); print v; exit }
		}
	' "$INI"
}

human_kb() {
	local kb="$1"
	if [ "$kb" -ge 1048576 ]; then awk -v k="$kb" 'BEGIN { printf "%.1f GB", k / 1048576 }'
	else echo "$((kb / 1024)) MB"; fi
}

# ---------- board ----------

check_board() {
	section "Board"
	local model
	model="$(tr -d '\0' 2>/dev/null < "$PROC/device-tree/model")"
	if [ -n "$model" ]; then res ok board.model "$model"
	else res warn board.model "unknown (no device tree model)"; fi

	# SDRAM: MiSTer Main keeps what the menu core detected in shared memory (read-only peek)
	if command -v devmem >/dev/null 2>&1 && [ -z "${DIAG_SKIP_DEVMEM:-}" ]; then
		local w b0 b1 cfg
		w="$(devmem 0x1FFFFF00 32 2>/dev/null)"
		if [ -n "$w" ]; then
			w=$((w))
			b0=$((w & 0xFF)); b1=$(((w >> 8) & 0xFF)); cfg=$(((w >> 24) & 0xFF))
			if [ "$b0" = 18 ] && [ "$b1" = 87 ]; then   # 0x12 0x57 marker
				case $((cfg & 7)) in
					1) res ok board.sdram "32 MB module" ;;
					3) res ok board.sdram "64 MB module" ;;
					7) res ok board.sdram "128 MB module" ;;
					*) res info board.sdram "module type $((cfg & 7))" ;;
				esac
			else
				res warn board.sdram "no SDRAM module reported (cores that need one won't start; reseat it)"
			fi
		else
			res info board.sdram "couldn't read MiSTer's SDRAM info"
		fi
	fi

	if [ -e "$SYS/class/rtc/rtc0" ]; then
		local t
		t="$(hwclock -r 2>/dev/null | head -n1)"
		res ok board.rtc "clock board present${t:+ ($t)}"
	else
		res info board.rtc "no clock board (time comes from the network)"
	fi

	# SuperStation One: its hub on USB port 1-1, the dock's hub on 1-1.1
	local d="$SYS/bus/usb/devices"
	if [ "$(cat "$d/1-1/bDeviceClass" 2>/dev/null)" = "09" ]; then
		if [ "$(cat "$d/1-1.1/bDeviceClass" 2>/dev/null)" = "09" ]; then
			res info board.superstation "SuperStation layout, dock connected"
		else
			res info board.superstation "SuperStation-style hub on port 1-1, no dock"
		fi
	fi

	local mem
	mem="$(awk '/^MemTotal:/ { print int($2 / 1024) }' "$PROC/meminfo" 2>/dev/null)"
	[ -n "$mem" ] && res ok board.ram "${mem} MB visible to Linux"
}

# ---------- cores ----------

check_cores() {
	section "Cores"
	local dir n total=0
	for dir in _Console _Computer _Arcade/cores _Other _Utility; do
		[ -d "$FAT/$dir" ] || continue
		n="$(find "$FAT/$dir" -maxdepth 1 -name '*.rbf' 2>/dev/null | wc -l)"
		total=$((total + n))
		res ok "cores.${dir//\//_}" "$n cores"
	done
	[ "$total" = 0 ] && res fail cores.none "no cores found in $FAT"

	# the same core more than once (old dated copies)
	local dups
	dups="$(find "$FAT"/_Console "$FAT"/_Computer "$FAT"/_Other "$FAT"/_Utility -maxdepth 1 -name '*_[0-9]*.rbf' 2>/dev/null \
		| sed -E 's|.*/||; s/_[0-9]{8}[^.]*\.rbf$//' | sort | uniq -d | tr '\n' ' ')"
	if [ -n "$dups" ]; then res warn cores.duplicates "more than one version of: $dups(MiSTer loads the newest; old copies only use space)"
	else res ok cores.duplicates "one version of each core"; fi

	for f in MiSTer menu.rbf; do
		if [ -f "$FAT/$f" ]; then res ok "cores.$f" "present ($(date -r "$FAT/$f" '+%Y-%m-%d' 2>/dev/null))"
		else res fail "cores.$f" "missing: MiSTer can't start without it"; fi
	done

	# BIOS files that cores need, for systems that have games
	bios() {   # bios <games folder> <file> <system>
		[ -d "$FAT/games/$1" ] || return
		find "$FAT/games/$1" -maxdepth 2 -type f ! -name '*.rom' ! -name '*.bin' 2>/dev/null | grep -q . || return
		if [ -f "$FAT/games/$1/$2" ]; then res ok "cores.bios.$1" "$3: $2 found"
		else res warn "cores.bios.$1" "$3: games/$1/$2 not found (the core needs it to start games)"; fi
	}
	bios PSX boot.rom PlayStation
	bios Saturn boot.rom Saturn
	bios TGFX16-CD cd_bios.rom "PC Engine CD"
	bios MegaCD boot.rom "Mega-CD"

	local c p
	c="$(cat /tmp/CORENAME 2>/dev/null)"; p="$(cat /tmp/FULLPATH 2>/dev/null)"
	res info cores.running "${c:-unknown}${p:+ ($p)}"
}

# ---------- os ----------

check_os() {
	section "OS"
	res info os.linux "MiSTer Linux $(cat /MiSTer.version 2>/dev/null || echo '?'), kernel $(uname -r)"
	local up load
	up="$(awk '{ s = int($1); printf "%dd %dh %dm", s / 86400, s % 86400 / 3600, s % 3600 / 60 }' "$PROC/uptime")"
	load="$(cut -d' ' -f1-3 "$PROC/loadavg")"
	res info os.uptime "up $up, load $load"
	# the DE10 has 2 cores: a sustained load over 2 means something is busy
	if awk -v l="${load%% *}" 'BEGIN { exit !(l > 2.0) }'; then
		res warn os.cpu "busy: $(top -bn1 2>/dev/null | awk 'NR > 4 && $0 ~ /[0-9]/ { print $NF; if (++n == 3) exit }' | tr '\n' ' ')"
	fi
	local avail
	avail="$(awk '/^MemAvailable:/ { print int($2 / 1024) }' "$PROC/meminfo")"
	if [ "${avail:-0}" -lt 32 ]; then res warn os.memory "${avail} MB free"; else res ok os.memory "${avail} MB free"; fi

	# Main can be stock (/media/fat/MiSTer) or a frontend's own build (e.g. ConsoleMode/MiSTer_ConsoleMode)
	local main_exe="" e t
	for e in "$PROC"/[0-9]*/exe; do
		t="$(readlink "$e" 2>/dev/null)"
		case "$t" in /media/fat/MiSTer*|/media/fat/*/MiSTer*) main_exe="$t"; break ;; esac
	done
	if [ -n "$main_exe" ]; then res ok os.main "MiSTer Main running: $main_exe"
	else res fail os.main "MiSTer Main isn't running"; fi

	# known boot messages that mean nothing (stock MiSTer Linux) are left out of the count:
	# - "check access for rdinit=/init failed": no startup ramdisk, every MiSTer boots without one
	# - "pcf8563_probe: write error": Linux tries the MiSTer RTC board's clock chip; a board with
	#   another clock chip (the SuperStation's, at 0x6f) fails that probe. Skipped only when a
	#   working clock is registered, so a real clock problem still shows.
	local known='check access for rdinit=/init failed'
	[ -e /sys/class/rtc/rtc0 ] && known="$known|rtc-pcf8563 .*pcf8563_probe: write error"
	local errlines errs skipped
	errlines="$(dmesg 2>/dev/null | grep -iE 'error|fail|oops|bug:|call trace')"
	errs="$(printf '%s\n' "$errlines" | grep -vE "$known" | grep -c .)"
	skipped="$(printf '%s\n' "$errlines" | grep -cE "$known")"
	local note=""
	[ "${skipped:-0}" -gt 0 ] && note=" ($skipped known harmless boot message(s) left out)"
	if [ "${errs:-0}" -gt 0 ]; then
		res warn os.kernel_log "$errs error line(s) since boot; newest: $(printf '%s\n' "$errlines" | grep -vE "$known" | tail -n1 | cut -c1-120)$note"
	else
		res ok os.kernel_log "no errors since boot$note"
	fi

	if [ -f "$INI" ]; then
		local dup
		dup="$(awk '{ sub(/\r$/, "") } /^\[/ { sec = $0; next } /^[ \t]*[;#]/ || !/=/ { next }
			{ split($0, kv, "="); k = kv[1]; gsub(/[ \t]/, "", k); if (seen[sec SUBSEP tolower(k)]++) print k }' "$INI" | sort -u | tr '\n' ' ')"
		if [ -n "$dup" ]; then res warn os.mister_ini "set more than once (the last one wins): $dup"
		else res ok os.mister_ini "no duplicate settings"; fi
	else
		res info os.mister_ini "no MiSTer.ini (defaults are used)"
	fi

	local us="$FAT/linux/user-startup.sh"
	if [ -f "$us" ]; then
		local entries
		entries="$(grep -cvE '^[[:space:]]*(#|$)' "$us")"
		res info os.user_startup "$entries line(s) run at boot; blocks: $(grep -oE '^# [a-z0-9 _-]+: BEGIN' "$us" | sed 's/^# //; s/: BEGIN//' | tr '\n' ',' | sed 's/,$//')"
	fi
}

# ---------- storage ----------

check_storage() {
	section "Storage"
	local m type opts size avail pct
	while read -r _ m type opts _; do
		case "$m" in "$FAT"|/media/usb*|"$FAT"/cifs*|/media/network*) ;; *) continue ;; esac
		m="${m//\\040/ }"
		read -r size avail <<< "$(df -kP "$m" 2>/dev/null | awk 'NR == 2 { print $2, $4 }')"
		[ "${size:-0}" -gt 0 ] || continue
		pct=$((avail * 100 / size))
		local level=ok
		[ "$pct" -lt 5 ] && level=warn
		[ "$avail" -lt 102400 ] && level=fail
		res "$level" "storage.${m##*/}" "$type, $(human_kb "$avail") free of $(human_kb "$size") ($pct%)"
		case ",$opts," in *,ro,*) res fail "storage.${m##*/}.rw" "mounted read-only: usually a file system error; check it on a PC" ;; esac
	done < "$PROC/mounts"
	local fserr
	fserr="$(dmesg 2>/dev/null | grep -ciE '(exfat|fat|ext4|ntfs3?)-fs .*error|i/o error|read-only')"
	if [ "${fserr:-0}" -gt 0 ]; then res warn storage.errors "$fserr storage error line(s) in the kernel log since boot"
	else res ok storage.errors "no storage errors since boot"; fi
}

# ---------- video ----------

check_video() {
	section "Video"
	local k v
	for k in video_mode vsync_adjust vscale_mode direct_video composite_sync vga_scaler forced_scandoubler hdmi_limited dvi_mode fb_terminal; do
		v="$(ini_get "$k")"
		[ -n "$v" ] && res info "video.$k" "$v"
	done
	[ "$(ini_get direct_video)" = 1 ] && [ "$(ini_get dvi_mode)" = 1 ] && \
		res warn video.combo "direct_video and dvi_mode are both on: dvi_mode also turns HDMI audio off"
	local fb="$SYS/class/graphics/fb0"
	if [ -d "$fb" ]; then
		res info video.framebuffer "$(tr ',' 'x' < "$fb/virtual_size" 2>/dev/null) at $(cat "$fb/bits_per_pixel" 2>/dev/null) bpp"
	fi
}

# ---------- sound ----------

check_sound() {
	section "Sound"
	local k v
	for k in hdmi_audio_96k dvi_mode volumectl; do
		v="$(ini_get "$k")"
		[ -n "$v" ] && res info "sound.$k" "$v"
	done
	[ "$(ini_get dvi_mode)" = 1 ] && res warn sound.hdmi "dvi_mode=1: no sound over HDMI"
	if [ -f "$PROC/asound/cards" ] && grep -q '^ *[0-9]' "$PROC/asound/cards"; then
		res info sound.usb "$(grep -E '^ *[0-9]' "$PROC/asound/cards" | sed -E 's/^ *[0-9]+ \[[^]]*\]: //' | tr '\n' ';')"
	else
		res info sound.usb "no USB audio devices (MiSTer's own sound goes through the FPGA)"
	fi
}

# ---------- input ----------

check_input() {
	section "Input"
	local names
	names="$(awk -F'"' '/^N: Name=/ { n = $2 } /^H: Handlers=/ && /event/ && (/js/ || /kbd/) { print n }' "$PROC/bus/input/devices" 2>/dev/null)"
	if [ -n "$names" ]; then
		while IFS= read -r n; do res ok input.device "$n"; done <<< "$names"
	else
		res warn input.device "no controllers or keyboards connected"
	fi
	local maps
	maps="$(find "$FAT/config/inputs" -name '*.map' 2>/dev/null | wc -l)"
	res info input.mappings "$maps controller mapping file(s) in config/inputs"
	[ -f "$FAT/linux/gamecontrollerdb/gamecontrollerdb.txt" ] && res ok input.gamecontrollerdb "present"
}

# ---------- network ----------

check_network() {
	section "Network"
	local ifs gw dns
	ifs="$(ip -4 -o addr show scope global 2>/dev/null | awk '{ print $2 " " $4 }' | tr '\n' ';')"
	if [ -n "$ifs" ]; then res ok network.address "$ifs"; else res warn network.address "no IPv4 address (not connected)"; fi
	gw="$(ip route show default 2>/dev/null | awk '{ print $3; exit }')"
	if [ -n "$gw" ]; then
		if run_limited 3 ping -c1 -W2 "$gw"; then res ok network.gateway "$gw answers"
		else res warn network.gateway "$gw doesn't answer ping"; fi
	else
		res warn network.gateway "no default route"
	fi
	dns="$(awk '/^nameserver/ { print $2 }' /etc/resolv.conf 2>/dev/null | tr '\n' ' ')"
	res info network.dns "${dns:-none}"
	if [ -n "$gw" ]; then
		mkdir -p "$DTMP"
		RL_OUT="$DTMP/diag.ping" run_limited 4 ping -c1 -W3 github.com
		if grep -q '^PING' "$DTMP/diag.ping" 2>/dev/null; then res ok network.dns_lookup "names resolve"
		else res warn network.dns_lookup "can't look up github.com (DNS)"; fi
		rm -f "$DTMP/diag.ping"
	fi
	if command -v iw >/dev/null 2>&1; then
		local w sig wp
		for wp in "$SYS"/class/net/*; do
			w="${wp##*/}"
			[ -d "$wp/wireless" ] || continue
			sig="$(iw dev "$w" link 2>/dev/null | awk '/SSID/ { s = $2 } /signal/ { g = $2 } END { if (s) print s " " g " dBm" }')"
			if [ -n "$sig" ]; then
				local dbm="${sig##* }"; dbm="${sig% dBm}"; dbm="${dbm##* }"
				if [ "${dbm%.*}" -lt -75 ] 2>/dev/null; then res warn "network.wifi" "$w: $sig (weak)"
				else res ok "network.wifi" "$w: $sig"; fi
			else
				res info "network.wifi" "$w: not connected"
			fi
		done
	fi
	grep -qE ' cifs | smb3 ' "$PROC/mounts" && res ok network.share "network share mounted"
}

# ---------- usb ----------

check_usb() {
	section "USB"
	local d n=0 name
	for d in "$SYS"/bus/usb/devices/*; do
		[ -f "$d/idVendor" ] || continue
		[ "$(cat "$d/bDeviceClass" 2>/dev/null)" = "09" ] && continue
		case "${d##*/}" in usb*) continue ;; esac
		n=$((n + 1))
		name="$(cat "$d/manufacturer" 2>/dev/null) $(cat "$d/product" 2>/dev/null)"
		res info "usb.${d##*/}" "$(cat "$d/idVendor"):$(cat "$d/idProduct") ${name# } ($(cat "$d/speed" 2>/dev/null) Mb/s)"
		if [ "$(cat "$d/idVendor")" = "152d" ]; then
			res info usb.nvme_bridge "JMicron NVMe bridge on ${d##*/}$([ -f /etc/jms583-phantom-guard.sh ] && echo ', phantom-drive guard installed in Linux')"
		fi
	done
	[ "$n" = 0 ] && res info usb.devices "no USB devices besides hubs"
	local pw rs
	pw="$(dmesg 2>/dev/null | grep -ciE 'over-current|under-?voltage')"
	rs="$(dmesg 2>/dev/null | grep -ciE 'device descriptor read.*error|unable to enumerate|reset (high|super|full)-speed usb device')"
	if [ "${pw:-0}" -gt 0 ]; then res fail usb.power "$pw over-current/voltage event(s) since boot: check the power supply and hubs"
	else res ok usb.power "no power problems since boot"; fi
	if [ "${rs:-0}" -ge 5 ]; then res warn usb.resets "$rs USB resets since boot: a loose cable or weak power"
	else res ok usb.resets "${rs:-0} USB resets since boot"; fi
}

# ---------- console mode ----------

check_consolemode() {
	section "Console Mode"
	local cm="$FAT/ConsoleMode" s
	if [ ! -x "$cm/ConsoleMode_arm" ]; then
		res info consolemode.installed "not installed (stock MiSTer or another frontend)"
		return 0
	fi
	if pidof ConsoleMode_arm >/dev/null 2>&1; then res ok consolemode.running "running"
	else res info consolemode.running "not running now (a game or another core has the screen)"; fi
	pidof MiSTer_ConsoleMode >/dev/null 2>&1 && res ok consolemode.main "Console Mode's MiSTer Main running"
	[ -f "$cm/config.ini" ] || res warn consolemode.config "config.ini is missing: Console Mode starts with its defaults"
	if [ -x "$TOOLS/cmremote" ]; then
		if s="$("$TOOLS/cmremote" info 2>/dev/null)" && [ -n "$s" ]; then
			res ok consolemode.app_link "app link: $(echo "$s" | grep -E '^(on|listening|port)=' | tr '\n' ' ')"
		else res warn consolemode.app_link "the app link (cmremote) isn't running: the PC and phone apps can't reach the SuperStation"; fi
	fi
	[ -x "$TOOLS/cmtrim.sh" ] && res info consolemode.trim "$("$TOOLS/cmtrim.sh" status 2>/dev/null | grep -E '^(mode|schedule|last_error)=' | tr '\n' ' ')"
	[ -x "$TOOLS/modules/update-guard/update-guard.sh" ] && res info consolemode.restore_points "$(ls -d "$TOOLS/backups/restore-points"/*/ 2>/dev/null | wc -l) restore point(s) of MiSTer and Linux"
	[ -d "$cm/Logs" ] && s="$(grep -il 'error\|fail' "$cm/Logs"/*.log 2>/dev/null | head -3 | tr '\n' ' ')" && [ -n "$s" ] && res info consolemode.logs "errors mentioned in: $s"
	return 0
}

# ---------- commands ----------

run_areas() {
	local a
	for a in "$@"; do
		case " $AREAS " in
			*" $a "*) "check_$a" ;;
			*) echo "unknown area: $a (areas: $AREAS)" >&2; return 2 ;;
		esac
	done
	if [ "$KV" = 1 ]; then
		local overall=ok
		[ "$WARNS" -gt 0 ] && overall=warn
		[ "$FAILS" -gt 0 ] && overall=fail
		echo "summary=$overall|$FAILS problem(s), $WARNS warning(s)"
	else
		printf '\n%d problem(s), %d warning(s)\n' "$FAILS" "$WARNS"
	fi
}

cmd_report() {
	local out
	out="$LOGS/diag-$(date '+%Y%m%d-%H%M%S').txt"
	mkdir -p "${out%/*}"
	{
		echo "Console Mode diag $VERSION, $(date '+%Y-%m-%d %H:%M:%S')"
		# shellcheck disable=SC2086
		run_areas $AREAS
	} > "$out" 2>&1
	echo "$out"
}

# copy a settings file with secrets blanked
redacted() {
	sed -E 's/^([[:space:]]*(PASSWORD|password|psk|PSK|pass|api_?key|APIKEY|ssid_password)[[:space:]]*=).*/\1 (removed)/' "$1"
}

cmd_bundle() {
	local stamp dir out f
	stamp="$(date '+%Y%m%d-%H%M%S')"
	dir="$DTMP/diag-$stamp"
	out="$LOGS/diag-bundle-$stamp.tar.gz"
	mkdir -p "$dir/settings" "$dir/logs" "${out%/*}"
	cp "$(cmd_report)" "$dir/report.txt"
	dmesg > "$dir/logs/dmesg.txt" 2>/dev/null
	cp "$DTMP"/*.log "$dir/logs/" 2>/dev/null
	cp "$TOOLS"/*.log "$LOGS"/*.log "$dir/logs/" 2>/dev/null
	# Console Mode's logs: its scraper puts the ScreenScraper login in URLs, so those values are taken out
	mkdir -p "$dir/logs/consolemode"
	for f in "$FAT/ConsoleMode/Logs"/*.log; do
		[ -f "$f" ] && sed -E 's/((ss|dev)(id|password|user|pass)|apikey|api_key|password)=[^&" ]*/\1=(removed)/Ig' "$f" > "$dir/logs/consolemode/${f##*/}"
	done
	rm -f "$dir"/logs/diag-*.txt
	for f in "$FAT/MiSTer.ini" "$FAT"/MiSTer_*.ini "$FAT/downloader.ini" "$FAT/Scripts/cifs_mount.ini" "$FAT/linux/user-startup.sh" \
	         "$FAT/ConsoleMode/config.ini" "$TOOLS/update-guard.ini"; do
		[ -f "$f" ] && redacted "$f" > "$dir/settings/${f##*/}"
	done
	# WiFi passwords live in wpa_supplicant.conf: only the network names go in
	[ -f "$FAT/linux/wpa_supplicant.conf" ] && grep -E '^[[:space:]]*ssid=' "$FAT/linux/wpa_supplicant.conf" > "$dir/settings/wifi-networks.txt"
	cat "$PROC/bus/input/devices" > "$dir/logs/input-devices.txt" 2>/dev/null
	# controllers: what the menu got from each (the input watch), and their MiSTer mappings
	mkdir -p "$dir/settings/inputs" && cp "$FAT"/config/inputs/*.map "$dir/settings/inputs/" 2>/dev/null
	"$TOOLS/cmmod" api '{"cmd":"controllers"}' > "$dir/logs/controllers.json" 2>/dev/null
	"$TOOLS/cmtrim.sh" status > "$dir/logs/trim.txt" 2>/dev/null
	"$TOOLS/sd_offline_repair.sh" status > "$dir/logs/sd-repair.txt" 2>/dev/null
	lsusb > "$dir/logs/lsusb.txt" 2>/dev/null
	cat "$PROC/mounts" > "$dir/logs/mounts.txt"
	tar -czf "$out" -C "${dir%/*}" "${dir##*/}" && rm -rf "$dir"
	echo "$out"
}

cmd_input() {
	# event: 32-bit ARM layout, 16 bytes: sec(4) usec(4) type(2) code(2) value(4)
	local devs=() d
	for d in /dev/input/event*; do [ -e "$d" ] && devs+=("$d"); done
	[ "${#devs[@]}" -gt 0 ] || { echo "No input devices."; return 1; }
	echo "Press buttons and move sticks; Ctrl-C to stop."
	for d in "${devs[@]}"; do
		local n
		n="$(cat "$SYS/class/input/${d##*/}/device/name" 2>/dev/null)"
		( od -An -v -w16 -tu2 < "$d" | while read -r _ _ _ _ type code v1 v2; do
			[ "$type" = 0 ] && continue
			value=$(( v1 | (v2 << 16) )); [ "$value" -ge 2147483648 ] && value=$((value - 4294967296))
			case "$type" in
				1) case "$value" in 1) state=down ;; 0) state=up ;; *) state=repeat ;; esac
				   printf '%-28s button %-4s %s\n' "${n:-${d##*/}}" "$code" "$state" ;;
				3) printf '%-28s axis   %-4s %s\n' "${n:-${d##*/}}" "$code" "$value" ;;
			esac
		done ) &
	done
	trap 'kill $(jobs -p) 2>/dev/null; exit 0' INT TERM
	wait
}

cmd_speed() {
	local target="${1:-$FAT}" f mb=64 t0 t1 w r
	f="$target/.cm-speedtest.$$"
	echo "Testing $(df -h "$target" | awk 'NR == 2 { print $1 " on " $6 }') with ${mb} MB..."
	sync
	t0="$(date +%s%N)"
	dd if=/dev/zero of="$f" bs=1M count=$mb conv=fsync 2>/dev/null || { echo "Write failed."; rm -f "$f"; return 1; }
	t1="$(date +%s%N)"
	w=$(( mb * 1000000000 / ((t1 - t0) ? (t1 - t0) : 1) ))
	echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
	t0="$(date +%s%N)"
	dd if="$f" of=/dev/null bs=1M 2>/dev/null
	t1="$(date +%s%N)"
	r=$(( mb * 1000000000 / ((t1 - t0) ? (t1 - t0) : 1) ))
	rm -f "$f"
	echo "write ${w} MB/s, read ${r} MB/s"
	[ "$w" -lt 5 ] && echo "Writes are slow: saves and installs will take a while on this drive."
	return 0
}

cmd_edid() {
	# The HDMI chip (ADV7513) keeps the TV's EDID at I2C address 0x3F once MiSTer Main has read it.
	# The bus is given explicitly: probing other buses can upset devices that answer by mistake.
	local bus="$1"
	if [ -z "$bus" ]; then
		echo "Usage: diag.sh edid <bus>   (I2C buses here: $(find /dev -maxdepth 1 -name 'i2c-*' 2>/dev/null | sed 's|/dev/i2c-||' | sort -n | tr '\n' ' '))"
		return 1
	fi
	echo "EDID from the HDMI chip on I2C bus $bus:"
	i2cdump -y "$bus" 0x3f b
}

[ "${1:-}" = "--kv" ] && { KV=1; shift; }
case "${1:-}" in
	report) cmd_report ;;
	bundle) cmd_bundle ;;
	input)  cmd_input ;;
	speed)  shift; cmd_speed "$@" ;;
	edid)   shift; cmd_edid "$@" ;;
	version) echo "$VERSION" ;;
	-h|--help|help) sed -n '2,19p' "$SELF" | sed 's/^# \{0,1\}//' ;;
	"") [ "$KV" = 0 ] && echo "Console Mode diag $VERSION"
	    # shellcheck disable=SC2086
	    run_areas $AREAS ;;
	*) run_areas "$@" ;;
esac
