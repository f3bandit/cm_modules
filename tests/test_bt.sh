#!/bin/sh
# The Bluetooth module through cmmod, with a pretend btctl and bluetoothctl. Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake.
CM=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
D=$(cd "$(dirname "$0")" && pwd)/bt
export CMOD_BTCTL="$D/btctl" CMOD_BLUETOOTHCTL="$D/bluetoothctl" CMOD_BT_SYS="$D/sys"
fail=0
check() { if eval "$2" >/dev/null 2>&1; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
check "list" '"$CM" bt | grep -q "^device=E4:17:D8:00:00:01|8BitDo Pro 2|connected|input-gaming" && "$CM" bt | grep -q "^device=00:1F:E2:00:00:02|Wireless Controller|-|"'
check "pair" '"$CM" bt pair | grep -q "^message=Paired 8BitDo Pro 2" && "$CM" bt pair | grep -q "^ok=1"'
check "forget" '"$CM" bt forget e4:17:d8:00:00:01'
check "bad address" '! "$CM" bt forget nope'
check "no adapter" 'CMOD_BT_SYS=/nonexistent "$CM" bt | grep -q "^present=0"'
[ $fail = 0 ] && echo "bt: all passed"
exit $fail
