#!/bin/sh
# The WiFi module through cmmod, with a pretend iw and wpa_passphrase. Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake.
CM=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
D=$(cd "$(dirname "$0")" && pwd)/wifi
export PATH="$D:$PATH" CMOD_IW="$D/iw" CMOD_WPA_CONF="$T/wpa_supplicant.conf" CMOD_WPA_PID="$T/none.pid" CMOD_HOME="$T/home"
printf 'ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\n' > "$CMOD_WPA_CONF"
fail=0
check() { if eval "$2" >/dev/null 2>&1; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
check "status" '"$CM" wifi | grep -q "^connected=1" && "$CM" wifi | grep -q "^ssid=Home Net"'
check "scan" '"$CM" wifi scan | grep -q "^network=Home Net|-48|2.4+5|secure" && "$CM" wifi scan | grep -q "^network=Cafe|-75|2.4|open"'
check "connect" '"$CM" wifi connect "Home Net" "hunter22" && grep -q "ssid=\"Home Net\"" "$CMOD_WPA_CONF"'
check "no password kept" '! grep -q hunter22 "$CMOD_WPA_CONF"'
check "connect open" '"$CM" wifi connect Cafe && "$CM" wifi | grep -q "^saved=Cafe|open"'
check "order" '"$CM" wifi | grep "^saved=" | head -1 | grep -q Cafe && "$CM" wifi down Cafe && "$CM" wifi | grep "^saved=" | head -1 | grep -q "Home Net"'
check "backed up" '[ "$(ls "$CMOD_HOME/backups/wifi" | wc -l)" -ge 3 ]'
check "forget" '"$CM" wifi forget Cafe && ! "$CM" wifi | grep -q "^saved=Cafe"'
check "undo" '"$CM" wifi undo && "$CM" wifi | grep -q "^saved=Cafe"'
check "short password" '! "$CM" wifi connect "Home Net" short'
[ $fail = 0 ] && echo "wifi: all passed"
exit $fail
