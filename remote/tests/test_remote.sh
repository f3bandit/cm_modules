#!/bin/sh
# The app link against a pretend SD card and USB drive: library commands, edit, pairing, keys, box art.
# Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see ../../LICENSE.md).
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
check "systems" '[ "$(api "{\"cmd\":\"systems\"}" | grep -o "\"games\":[0-9]*" | tr "\n" " ")" = "\"games\":4 \"games\":6 \"games\":2 " ]'
check "folder of discs gets their art" 'api "{\"cmd\":\"browse\",\"system\":\"PLAYSTATION\"}" | grep -q "\"title\":\"Star Ocean - The Second Story\"[^}]*\"art_id\":[0-9]"'
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
check "status" 'curl -s -H "Authorization: Bearer $TOK" -XPOST $M/api -d "{\"cmd\":\"status\"}" | grep -q "\"games\":12"'
python3 -c "
import zlib,struct
w,h=900,1200
raw=b''.join(b'\x00'+bytes([(x+y)%256 for x in range(w) for _ in (0,1,2)]) for y in range(h))
c=lambda t,d: struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
open('$T/big.png','wb').write(b'\x89PNG\r\n\x1a\n'+c(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+c(b'IDAT',zlib.compress(raw))+c(b'IEND',b''))"
check "art put" 'curl -s -H "Authorization: Bearer $TOK" -T "$T/big.png" $M/art/$ZID | grep -q "\"ok\":true"'
check "art 512 wide" 'python3 -c "import struct,sys;d=open(sys.argv[1],\"rb\").read(24);sys.exit(struct.unpack(\">II\",d[16:24])!=(512,682))" "$T/fx/usb0/games/GAMEBOY/media/Zelda & Link.png"'
check "art get" '[ "$(curl -s -o /dev/null -w "%{http_code} %{content_type}" -H "Authorization: Bearer $TOK" $M/art/$ZID)" = "200 image/png" ]'
check "arcade art in /media/fat/media" 'api "{\"cmd\":\"scrape_todo\",\"system\":\"Arcade\",\"all\":true}" | grep -q "\"title\":\"Galaga\"[^}]*\"has_art\":false.*\"has_art\":true" || api "{\"cmd\":\"scrape_todo\",\"system\":\"Arcade\",\"all\":true}" | grep -q "\"has_art\":true.*\"title\":\"Galaga\"[^}]*\"has_art\":false"'
GID=$("$B" api '{"cmd":"games","q":"galaga"}' | grep -o '"id":[^,]*' | head -1 | cut -d: -f2 | tr -d ' \t')
check "arcade art put" 'curl -s -H "Authorization: Bearer $TOK" -T "$T/big.png" $M/art/$GID | grep -q "\"ok\":true" && [ -f "$T/fx/fat/media/Galaga.png" ] && [ ! -d "$T/fx/fat/_Arcade/media" ]'
python3 -c "
import zlib,struct,sys
def png(p,w,h):
    raw=b''.join(b'\x00'+bytes([(x*7+y)%256 for x in range(w) for _ in (0,1,2)]) for y in range(h))
    c=lambda t,d: struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
    open(p,'wb').write(b'\x89PNG\r\n\x1a\n'+c(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+c(b'IDAT',zlib.compress(raw))+c(b'IEND',b''))
png('$T/fx/usb0/docs/GAMEBOY/Artwork/super mario land (world).png',60,80)
png('$T/fx/fat/docs/GAMEBOY/Titles/B.png',60,80)
png('$T/fx/fat/docs/Arcade/Artwork/galaga.png',60,80)"
rm -f "$T/fx/fat/media/Galaga.png"
check "art from Update All's artwork" 'api "{\"cmd\":\"art_docs\"}" | grep -q "\"set\":2" && [ -f "$T/fx/usb0/games/GAMEBOY/media/Super Mario Land (World).png" ] && [ -f "$T/fx/fat/media/Galaga.png" ]'
check "not from Titles" '! ls "$T"/fx/usb0/games/GAMEBOY/*/media/B.png "$T"/fx/usb0/games/GAMEBOY/media/B.png 2>/dev/null | grep -q .'
mkdir -p "$T/fx/fat/screenshots/SNES"; cp "$T/big.png" "$T/fx/fat/screenshots/SNES/20261102_120000-Zelda.png"
check "shots list" 'api "{\"cmd\":\"shots\"}" | grep -q "\"core\":\"SNES\",\"name\":\"20261102_120000-Zelda.png\""'
check "shot get" '[ "$(curl -s -o /dev/null -w "%{http_code} %{content_type} %{size_download}" -H "Authorization: Bearer $TOK" "$M/shot/SNES/20261102_120000-Zelda.png")" = "200 image/png $(wc -c < "$T/big.png" | tr -d " ")" ]'
check "shot outside" '[ "$(curl -s -o /dev/null -w %{http_code} -H "Authorization: Bearer $TOK" "$M/shot/..%2F..%2FConsoleMode/config.ini")" = 404 ]'
check "shot no key" '[ "$(curl -s -o /dev/null -w %{http_code} "$M/shot/SNES/20261102_120000-Zelda.png")" = 401 ]'
check "shot delete" 'api "{\"cmd\":\"shots\",\"do\":\"delete\",\"core\":\"SNES\",\"name\":\"20261102_120000-Zelda.png\"}" | grep -q "\"ok\":true" && [ ! -f "$T/fx/fat/screenshots/SNES/20261102_120000-Zelda.png" ]'
mkdir -p "$CMOD_HOME/backups/memory-cards" && head -c 131072 /dev/zero > "$CMOD_HOME/backups/memory-cards/card 1.mcd"
check "backup get" '[ "$(curl -s -H "Authorization: Bearer $TOK" "$M/backup/memory-cards/card%201.mcd" | wc -c)" = 131072 ]'
check "backup no key" '[ "$(curl -s -o /dev/null -w %{http_code} "$M/backup/memory-cards/card%201.mcd")" = 401 ]'
check "backup outside" '[ "$(curl -s -o /dev/null -w %{http_code} -H "Authorization: Bearer $TOK" "$M/backup/memory-cards/..%2F..%2Fremote.ini")" = 404 ]'
mkdir -p "$CMOD_HOME/backups/themes/t1/20261103-091500" "$CMOD_HOME/backups/restore-points/store" && echo x > "$CMOD_HOME/backups/themes/t1/20261103-091500/cmtheme.xml" && echo l > "$CMOD_HOME/backups/restore-points/store/linux.img"
check "backup files listed" 'api "{\"cmd\":\"backups\",\"do\":\"files\",\"kind\":\"themes\"}" | grep -q "\"path\":\"t1/20261103-091500/cmtheme.xml\",\"bytes\":2"'
check "backup in a subfolder" '[ "$(curl -s -H "Authorization: Bearer $TOK" "$M/backup/themes/t1/20261103-091500/cmtheme.xml")" = x ]'
check "restore points stay" 'api "{\"cmd\":\"backups\",\"do\":\"files\",\"kind\":\"restore-points\"}" | grep -q "\"ok\":false" && [ "$(curl -s -o /dev/null -w %{http_code} -H "Authorization: Bearer $TOK" "$M/backup/restore-points/store/linux.img")" = 404 ]'
check "backup dot path" '[ "$(curl -s -o /dev/null -w %{http_code} -H "Authorization: Bearer $TOK" "$M/backup/themes/t1/.%2E/x")" = 404 ]'
check "wifi" 'api "{\"cmd\":\"wifi\",\"do\":\"connect\",\"ssid\":\"Cafe\"}" | grep -q "\"saved\":\[{\"ssid\":\"Cafe\""'
check "api on the socket" '"$(dirname "$B")/test_cmmod" api "{\"cmd\":\"wifi\"}" | grep -q "\"saved\":\[{\"ssid\":\"Cafe\""'
printf '[MiSTer]\nvideo_mode=8\nvsync_adjust=0\n' > "$CMOD_TEST_ROOT/fat/MiSTer.ini"; mkdir -p "$CMOD_TEST_ROOT/fat/ConsoleMode"; printf '[APPLICATION]\nvolume=5\n' > "$CMOD_TEST_ROOT/fat/ConsoleMode/config.ini"
check "ini read" 'api "{\"cmd\":\"ini\"}" | grep -q "video_mode=8"'
check "ini save kept" 'api "{\"cmd\":\"ini\",\"do\":\"save\",\"text\":\"[MiSTer]\\nvideo_mode=9\\n\"}" | grep -q "\"kept\":\"MiSTer.ini." && grep -q video_mode=9 "$CMOD_TEST_ROOT/fat/MiSTer.ini" && ls "$CMOD_HOME/backups/ini" | grep -q "^MiSTer.ini."'
check "console mode ini" 'api "{\"cmd\":\"ini\",\"file\":\"ConsoleMode.ini\"}" | grep -q "volume=5"'
check "vset" 'api "{\"cmd\":\"vset\"}" | grep -q "video_mode"'
check "scraper set" 'api "{\"cmd\":\"scraper_accounts\",\"do\":\"set\",\"ssid\":\"bob\",\"sspassword\":\"pw1\"}" | grep -q "\"ss_user\":\"bob\",\"ss_password_set\":true" && grep -q "^sspassword=pw1" "$CMOD_TEST_ROOT/fat/ConsoleMode/screenscraper.txt"'
mkdir -p "$CMOD_TEST_ROOT/fat/Scripts/.config/ra_viewer"; printf '[retroachievements]\nusername = old\napi_key = \n' > "$CMOD_TEST_ROOT/fat/Scripts/.config/ra_viewer/config.ini"; printf '# RA\nusername=old\npassword=\nshow_popup=1\n' > "$CMOD_TEST_ROOT/fat/retroachievements.cfg"
check "ra set" 'api "{\"cmd\":\"scraper_accounts\",\"do\":\"set\",\"ra_user\":\"bob\",\"ra_key\":\"K123\",\"ra_password\":\"pw9\"}" | grep -q "\"ra_user\":\"bob\",\"ra_password_set\":true,\"ra_key_set\":true" && grep -q "^password=pw9" "$CMOD_TEST_ROOT/fat/retroachievements.cfg" && grep -q "^show_popup=1" "$CMOD_TEST_ROOT/fat/retroachievements.cfg" && grep -q "^api_key=K123" "$CMOD_TEST_ROOT/fat/Scripts/.config/ra_viewer/config.ini" && grep -q "^username=bob" "$CMOD_TEST_ROOT/fat/Scripts/.config/ra_viewer/config.ini"'
check "ra no secrets back" '! api "{\"cmd\":\"scraper_accounts\"}" | grep -q "pw9\|K123"'
check "scraper no password back" '! api "{\"cmd\":\"scraper_accounts\"}" | grep -q pw1'
export CMOD_UINPUT="$T/uinput"; : > "$CMOD_UINPUT"
check "remote pad" 'api "{\"cmd\":\"vinput\",\"pad\":\"a\"}" | grep -q "\"pad\":true" && grep -aq "Console Mode Remote Keyboard" "$CMOD_UINPUT" && grep -aq "X-Box 360 pad" "$CMOD_UINPUT"'
check "remote text" 'n=$(wc -c < "$CMOD_UINPUT"); api "{\"cmd\":\"vinput\",\"text\":\"Hi!\"}" | grep -q "\"ok\":true" && [ $(wc -c < "$CMOD_UINPUT") -gt $n ]'
check "remote bad pad" 'api "{\"cmd\":\"vinput\",\"pad\":\"z\"}" | grep -q "pad: a, b"'
mkdir -p "$CMOD_TEST_ROOT/fat/Scripts" "$CMOD_TEST_ROOT/fat/linux"; printf '#!/bin/sh\necho mounted\n' > "$CMOD_TEST_ROOT/fat/Scripts/cifs_mount.sh"; cp "$CMOD_TEST_ROOT/fat/Scripts/cifs_mount.sh" "$CMOD_TEST_ROOT/fat/Scripts/cifs_umount.sh"
printf 'SERVER="old"\nSHARE="x"\nPASSWORD="secret1"\nEXTRA_THING="keep"\n' > "$CMOD_TEST_ROOT/fat/Scripts/cifs_mount.ini"
printf '#!/bin/sh\n# cifs_mount: BEGIN managed boot mount\nx &\n# cifs_mount: END managed boot mount\necho other\n' > "$CMOD_TEST_ROOT/fat/linux/user-startup.sh"
check "cifs set" 'api "{\"cmd\":\"cifs\",\"do\":\"set\",\"settings\":{\"SERVER\":\"nas.local\",\"SHARE\":\"games\",\"MOUNT_AT_BOOT\":\"false\"}}" | grep -q "\"SERVER\":\"nas.local\"" && grep -q "^PASSWORD=\"secret1\"" "$CMOD_TEST_ROOT/fat/Scripts/cifs_mount.ini" && grep -q "^EXTRA_THING=\"keep\"" "$CMOD_TEST_ROOT/fat/Scripts/cifs_mount.ini" && ! grep -q "cifs_mount: BEGIN" "$CMOD_TEST_ROOT/fat/linux/user-startup.sh" && grep -q "echo other" "$CMOD_TEST_ROOT/fat/linux/user-startup.sh"'
check "cifs no password back" '! api "{\"cmd\":\"cifs\"}" | grep -q secret1 && api "{\"cmd\":\"cifs\"}" | grep -q "\"password_set\":true"'
check "cifs bad value" 'api "{\"cmd\":\"cifs\",\"do\":\"set\",\"settings\":{\"SERVER\":\"a\$(x)\",\"SHARE\":\"s\"}}" | grep -q "character"'
TH="$CMOD_TEST_ROOT/fat/ConsoleMode/themes"; mkdir -p "$TH/lower-third/fonts"; printf '<theme><variables><a>1</a></variables></theme>' > "$TH/lower-third/cmtheme.xml"; printf '<themeCapabilities><themeName>Lower Third</themeName></themeCapabilities>' > "$TH/lower-third/capabilities.xml"; : > "$TH/lower-third/fonts/X.ttf"; printf '1234 lower-third/cmtheme.xml\n' > "$TH/.cm-bundled"
check "themes list" 'api "{\"cmd\":\"themes\"}" | grep -q "\"folder\":\"lower-third\",\"name\":\"Lower Third\",\"bundled\":true"'
check "theme bundled kept" 'api "{\"cmd\":\"themes\",\"do\":\"write\",\"folder\":\"lower-third\",\"file\":\"cmtheme.xml\",\"text\":\"x\"}" | grep -q "comes with Console Mode"'
check "theme copy and write" 'api "{\"cmd\":\"themes\",\"do\":\"copy\",\"folder\":\"lower-third\",\"to\":\"my-theme\"}" | grep -q "\"ok\":true" && [ -f "$TH/my-theme/fonts/X.ttf" ] && api "{\"cmd\":\"themes\",\"do\":\"write\",\"folder\":\"my-theme\",\"file\":\"art/dark/buttons/a.png\",\"data\":\"iVBORw0KGgo=\"}" | grep -q "\"ok\":true" && [ -f "$TH/my-theme/art/dark/buttons/a.png" ]'
check "theme bad path" 'api "{\"cmd\":\"themes\",\"do\":\"write\",\"folder\":\"my-theme\",\"file\":\"../../config.ini\",\"text\":\"x\"}" | grep -q "file:"'
check "theme use" 'api "{\"cmd\":\"themes\",\"do\":\"use\",\"folder\":\"my-theme\"}" | grep -q "\"ok\":true" && grep -q "^theme=my-theme" "$CMOD_TEST_ROOT/fat/ConsoleMode/config.ini" && grep -q "^volume=5" "$CMOD_TEST_ROOT/fat/ConsoleMode/config.ini"'
check "theme backed up before editing" 'K=$(find "$CMOD_HOME/backups/themes/my-theme" -name cmtheme.xml | head -1); [ -n "$K" ] && grep -q "<a>1</a>" "$K" && grep -q "before editing" "$(dirname "$K")/.cm-why"'
check "theme one backup per session" '[ $(api "{\"cmd\":\"themes\",\"do\":\"write\",\"folder\":\"my-theme\",\"file\":\"cmtheme.xml\",\"text\":\"<theme>v2</theme>\"}" >/dev/null; api "{\"cmd\":\"themes\",\"do\":\"backups\",\"folder\":\"my-theme\"}" | grep -o "\"stamp\"" | wc -l) = 1 ]'
check "theme restore" 'S=$(api "{\"cmd\":\"themes\",\"do\":\"backups\",\"folder\":\"my-theme\"}" | sed -n "s/.*\"stamp\":\"\([0-9-]*\)\".*/\1/p"); sleep 1; api "{\"cmd\":\"themes\",\"do\":\"restore\",\"folder\":\"my-theme\",\"stamp\":\"$S\"}" | grep -q "\"saved\"" && grep -q "<a>1</a>" "$TH/my-theme/cmtheme.xml" && [ ! -e "$TH/my-theme/.cm-why" ] && [ ! -d "$TH/my-theme.cm-old" ] && api "{\"cmd\":\"themes\",\"do\":\"backups\",\"folder\":\"my-theme\"}" | grep -q "before going back"'
check "theme restore bad stamp" 'api "{\"cmd\":\"themes\",\"do\":\"restore\",\"folder\":\"my-theme\",\"stamp\":\"../../x\"}" | grep -q "stamp:"'
check "theme delete" 'api "{\"cmd\":\"themes\",\"do\":\"delete\",\"folder\":\"my-theme\"}" | grep -q "\"ok\":true" && [ ! -d "$TH/my-theme" ]'
check "theme back after delete" 'api "{\"cmd\":\"themes\",\"do\":\"backups\",\"folder\":\"my-theme\"}" | grep -q "before deleting" && S=$(api "{\"cmd\":\"themes\",\"do\":\"backups\",\"folder\":\"my-theme\"}" | sed -n "s/^[^[]*\[{\"stamp\":\"\([0-9-]*\)\".*/\1/p") && api "{\"cmd\":\"themes\",\"do\":\"restore\",\"folder\":\"my-theme\",\"stamp\":\"$S\"}" | grep -q "\"ok\":true" && [ -f "$TH/my-theme/cmtheme.xml" ]'
check "theme builtin" 'api "{\"cmd\":\"themes\",\"do\":\"builtin\"}" | grep -q "\"was\":\"my-theme\"" && grep -q "^theme=ConsoleMode" "$CMOD_TEST_ROOT/fat/ConsoleMode/config.ini"'
check "backups lists themes" 'api "{\"cmd\":\"backups\"}" | grep -q "\"id\":\"themes\""'
check "proofs" 'curl -s "$M/info?nonce=00112233445566778899" | grep -q "\"proofs\":\[\"[0-9a-f]\{64\}\"\]"'
ID=$("$B" info | grep '^device=' | cut -d= -f2 | cut -d' ' -f1)
check "forget" '"$B" forget $ID | grep -q result=ok'
check "forgotten key" '[ "$(curl -s -o /dev/null -w %{http_code} -H "Authorization: Bearer $TOK" -XPOST $M/api -d "{\"cmd\":\"status\"}")" = 401 ]'
kill $PID 2>/dev/null; wait $PID 2>/dev/null
rm -rf "$T"
[ $fails = 0 ] && echo "remote: all passed" || echo "remote: $fails failed"
exit $fails
