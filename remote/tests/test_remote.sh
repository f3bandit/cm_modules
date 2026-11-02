#!/bin/sh
# The app link against a pretend SD card and USB drive: library commands, edit, pairing, keys, box art.
# Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../../LICENSE).
set -u
B=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
T=$(mktemp -d)
D=$(dirname "$0")
sh "$D/make_fixture.sh" "$T/fx"
export CMOD_TEST_ROOT=$T/fx CMOD_HOME=$T/tools CMREMOTE_SOCK=$T/ctl.sock
W=$(cd "$D/../../tests/wifi" && pwd)
export PATH="$W:$PATH" CMOD_IW="$W/iw" CMOD_WPA_CONF="$T/wpa_supplicant.conf" CMOD_WPA_PID="$T/none.pid"
mkdir -p "$CMOD_HOME"
PORT=18767
printf 'on = 1\nport = %s\nname = Console Mode\n' $PORT > "$CMOD_HOME/remote.ini"
fails=0
check() { if eval "$2"; then :; else echo "FAIL: $1"; fails=$((fails+1)); fi; }
api() { "$B" api "$1" | tr -d '\n\t'; }
check "systems" '[ "$(api "{\"cmd\":\"systems\"}" | grep -o "\"games\":[0-9]*" | tr "\n" " ")" = "\"games\":2 \"games\":6 " ]'
check "disc set and one-game folder" 'api "{\"cmd\":\"games\",\"system\":\"PLAYSTATION\"}" | grep -q "Castlevania (USA).*FF7"'
check "folder of games listed inside" 'api "{\"cmd\":\"games\",\"system\":\"GAMEBOY\"}" | grep -q "\"title\":\"A\".*\"title\":\"B\".*Super Mario Land"'
check "details from gamelist.xml" 'api "{\"cmd\":\"game\",\"id\":$("$B" api "{\"cmd\":\"games\",\"q\":\"tetris\"}" | grep -o "\"id\":[^,]*" | head -1 | cut -d: -f2 | tr -d " \t")}" | grep -q "Falling blocks & lines"'
ZID=$("$B" api '{"cmd":"games","q":"zelda"}' | grep -o '"id":[^,]*' | head -1 | cut -d: -f2 | tr -d ' \t')
check "edit" 'api "{\"cmd\":\"edit\",\"id\":$ZID,\"set\":{\"year\":\"1993\",\"description\":\"a <b>\"}}" | grep -q "\"year\":\"1993\""'
check "edit escaped" 'grep -q "<desc>a &lt;b&gt;</desc>" "$T/fx/usb0/games/GAMEBOY/gamelist.xml"'
check "bad year" 'api "{\"cmd\":\"edit\",\"id\":$ZID,\"set\":{\"year\":\"93\"}}" | grep -q "four digits"'
"$B" serve & PID=$!
sleep 1.5
M=http://127.0.0.1:$PORT
check "info" 'curl -s $M/info | grep -q "\"name\":\"Console Mode\""'
check "no key" '[ "$(curl -s -o /dev/null -w %{http_code} -XPOST $M/api -d "{\"cmd\":\"status\"}")" = 401 ]'
check "pairing closed" '[ "$(curl -s -o /dev/null -w %{http_code} -XPOST $M/pair -d "{\"code\":\"123456\",\"name\":\"x\"}")" = 403 ]'
CODE=$("$B" pair | grep '^code=' | cut -d= -f2)
check "wrong code" '[ "$(curl -s -o /dev/null -w %{http_code} -XPOST $M/pair -d "{\"code\":\"99999x\",\"name\":\"x\"}")" = 401 ]'
TOK=$(curl -s -XPOST $M/pair -d "{\"code\":\"$CODE\",\"name\":\"Test\"}" | grep -o '"token":"[0-9a-f]*"' | cut -d'"' -f4)
check "paired" '[ ${#TOK} = 64 ]'
check "status" 'curl -s -H "Authorization: Bearer $TOK" -XPOST $M/api -d "{\"cmd\":\"status\"}" | grep -q "\"games\":8"'
python3 -c "
import zlib,struct
w,h=900,1200
raw=b''.join(b'\x00'+bytes([(x+y)%256 for x in range(w) for _ in (0,1,2)]) for y in range(h))
c=lambda t,d: struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
open('$T/big.png','wb').write(b'\x89PNG\r\n\x1a\n'+c(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+c(b'IDAT',zlib.compress(raw))+c(b'IEND',b''))"
check "art put" 'curl -s -H "Authorization: Bearer $TOK" -T "$T/big.png" $M/art/$ZID | grep -q "\"ok\":true"'
check "art 512 wide" 'python3 -c "import struct,sys;d=open(sys.argv[1],\"rb\").read(24);sys.exit(struct.unpack(\">II\",d[16:24])!=(512,682))" "$T/fx/usb0/games/GAMEBOY/media/Zelda & Link.png"'
check "art get" '[ "$(curl -s -o /dev/null -w "%{http_code} %{content_type}" -H "Authorization: Bearer $TOK" $M/art/$ZID)" = "200 image/png" ]'
mkdir -p "$CMOD_HOME/backups/memory-cards" && head -c 131072 /dev/zero > "$CMOD_HOME/backups/memory-cards/card 1.mcd"
check "backup get" '[ "$(curl -s -H "Authorization: Bearer $TOK" "$M/backup/memory-cards/card%201.mcd" | wc -c)" = 131072 ]'
check "backup no key" '[ "$(curl -s -o /dev/null -w %{http_code} "$M/backup/memory-cards/card%201.mcd")" = 401 ]'
check "backup outside" '[ "$(curl -s -o /dev/null -w %{http_code} -H "Authorization: Bearer $TOK" "$M/backup/memory-cards/..%2F..%2Fremote.ini")" = 404 ]'
check "wifi" 'api "{\"cmd\":\"wifi\",\"do\":\"connect\",\"ssid\":\"Cafe\"}" | grep -q "\"saved\":\[{\"ssid\":\"Cafe\""'
check "api on the socket" '"$(dirname "$B")/test_cmmod" api "{\"cmd\":\"wifi\"}" | grep -q "\"saved\":\[{\"ssid\":\"Cafe\""'
printf '[MiSTer]\nvideo_mode=8\nvsync_adjust=0\n' > "$CMOD_TEST_ROOT/fat/MiSTer.ini"; mkdir -p "$CMOD_TEST_ROOT/fat/ConsoleMode"; printf '[APPLICATION]\nvolume=5\n' > "$CMOD_TEST_ROOT/fat/ConsoleMode/config.ini"
check "ini read" 'api "{\"cmd\":\"ini\"}" | grep -q "video_mode=8"'
check "ini save kept" 'api "{\"cmd\":\"ini\",\"do\":\"save\",\"text\":\"[MiSTer]\\nvideo_mode=9\\n\"}" | grep -q "\"kept\":\"MiSTer.ini." && grep -q video_mode=9 "$CMOD_TEST_ROOT/fat/MiSTer.ini" && ls "$CMOD_HOME/backups/ini" | grep -q "^MiSTer.ini."'
check "console mode ini" 'api "{\"cmd\":\"ini\",\"file\":\"ConsoleMode.ini\"}" | grep -q "volume=5"'
check "vset" 'api "{\"cmd\":\"vset\"}" | grep -q "video_mode"'
check "proofs" 'curl -s "$M/info?nonce=00112233445566778899" | grep -q "\"proofs\":\[\"[0-9a-f]\{64\}\"\]"'
ID=$("$B" info | grep '^device=' | cut -d= -f2 | cut -d' ' -f1)
check "forget" '"$B" forget $ID | grep -q result=ok'
check "forgotten key" '[ "$(curl -s -o /dev/null -w %{http_code} -H "Authorization: Bearer $TOK" -XPOST $M/api -d "{\"cmd\":\"status\"}")" = 401 ]'
kill $PID 2>/dev/null; wait $PID 2>/dev/null
rm -rf "$T"
[ $fails = 0 ] && echo "remote: all passed" || echo "remote: $fails failed"
exit $fails
