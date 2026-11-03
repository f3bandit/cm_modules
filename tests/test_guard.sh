#!/bin/sh
# update-guard's Linux guard: on by default (boot holds Linux updates), off when the user says, back on again
set -e
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
G="$(pwd)/modules/update-guard/update-guard.sh"
export CM_ROOT="$T/root" CM_TMP="$T/tmp" UPDATE_GUARD_INI="$T/update-guard.ini" UPDATE_GUARD_DOWNLOADER_INI="$T/downloader.ini" \
	UPDATE_GUARD_ROOTFS="$T/fs" UPDATE_GUARD_SD="$T/sd" UPDATE_GUARD_SAVED="$T/saved" UPDATE_GUARD_BACKUPS="$T/bk"
mkdir -p "$T/fs" "$T/sd" "$CM_ROOT"
printf '[MiSTer]\nupdate_linux = true\n\n[distribution_mister]\ndb_url = x\n' > "$T/downloader.ini"
v() { awk 'tolower($0)=="[mister]"{m=1;next} /^\[/{m=0} m && /update_linux/{sub(/.*= */,""); print}' "$T/downloader.ini"; }
bash "$G" boot >/dev/null 2>&1 || true
[ "$(v)" = false ] || { echo "FAIL: boot didn't hold Linux updates ($(v))"; exit 1; }
bash "$G" ui-state | grep -qx linux_guard=on || { echo "FAIL: ui-state guard"; exit 1; }
bash "$G" guard off >/dev/null
[ "$(v)" = true ] || { echo "FAIL: off didn't put update_linux back ($(v))"; exit 1; }
bash "$G" boot >/dev/null 2>&1 || true
[ "$(v)" = true ] || { echo "FAIL: boot held it again although the guard is off"; exit 1; }
bash "$G" ui-state | grep -qx linux_guard=off || { echo "FAIL: ui-state off"; exit 1; }
bash "$G" guard on >/dev/null
[ "$(v)" = false ] || { echo "FAIL: on didn't hold"; exit 1; }
grep -q '^\[distribution_mister\]' "$T/downloader.ini" && grep -q 'db_url = x' "$T/downloader.ini" || { echo "FAIL: downloader.ini damaged"; cat "$T/downloader.ini"; exit 1; }
[ "$(grep -ci '^\[mister\]' "$T/downloader.ini")" = 1 ] && [ "$(grep -c update_linux "$T/downloader.ini")" = 1 ] || { echo "FAIL: a second [mister] section or key: $(cat "$T/downloader.ini")"; exit 1; }
echo "guard: all passed"
