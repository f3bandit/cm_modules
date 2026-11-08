#!/bin/bash
# sound-level module and cmsfx: levels, the limiter, originals kept and put back, changed and removed sounds
# usage: bash tests/test_sound_level.sh <cmsfx binary>
set -u
B="$(readlink -f "$1")"; HERE="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
export SOUND_LEVEL_TOOLS="$T/tools" SOUND_LEVEL_DIR="$T/sfx" CMSFX="$B" CM_TMP="$T/tmp"
mkdir -p "$T/tools" "$T/sfx"
fail=0; ok() { echo "ok   $*"; }; bad() { echo "FAIL $*"; fail=1; }
python3 - "$T/sfx" <<'PY'
import math, struct, sys, wave, os
d = sys.argv[1]
def mk(name, bits, amp, fmt=1, ch=2, rate=22050, n=4000):
    frames = b''
    for i in range(n):
        x = amp * math.sin(2 * math.pi * 440 * i / rate)
        for c in range(ch):
            if fmt == 3: frames += struct.pack('<f', x)
            elif bits == 8: frames += bytes([int(round(x * 127)) + 128])
            elif bits == 16: frames += struct.pack('<h', int(round(x * 32767)))
            elif bits == 24: frames += struct.pack('<i', int(round(x * 8388607)))[:3]
    bps = bits // 8
    fmtck = struct.pack('<HHIIHH', fmt, ch, rate, rate * ch * bps, ch * bps, bits)
    data = b'RIFF' + struct.pack('<I', 4 + 8 + len(fmtck) + 8 + len(frames) + 8 + 4) + b'WAVE'
    data += b'fmt ' + struct.pack('<I', len(fmtck)) + fmtck + b'LIST' + struct.pack('<I', 4) + b'INFO'
    data += b'data' + struct.pack('<I', len(frames)) + frames
    open(os.path.join(d, name), 'wb').write(data)
mk('confirm.wav', 16, 0.5)
mk('back.wav', 8, 0.4, ch=1)
mk('move.WAV', 24, 0.6)
mk('loud.wav', 16, 0.95)
mk('float.wav', 32, 0.3, fmt=3)
open(os.path.join(d, 'jingle.mp3'), 'wb').write(b'ID3\x03\x00' + b'\x00' * 200)
PY
peak() { "$B" info "$1" | sed 's/.*peak=//'; }
md5() { md5sum < "$1" | cut -c1-32; }
declare -A o; for f in "$T"/sfx/*; do o["${f##*/}"]="$(md5 "$f")"; done

[ "$(peak "$T/sfx/confirm.wav")" = 0.500 ] && ok "info reads the peak" || bad "info peak $(peak "$T/sfx/confirm.wav")"
out="$(bash "$HERE/modules/sound-level/sound-level.sh" set 150)"
echo "$out" | grep -q '^sounds=5$' && ok "set 150: 5 WAV sounds set" || bad "set 150: $out"
echo "$out" | grep -q '^skipped=jingle.mp3$' && ok "the MP3 is skipped" || bad "mp3: $out"
p="$(peak "$T/sfx/confirm.wav")"; [ "$p" = 0.750 ] && ok "16-bit at 150%: 0.500 -> $p" || bad "16-bit 150% peak $p"
p="$(peak "$T/sfx/move.WAV")"; awk -v p="$p" 'BEGIN{exit !(p>0.88 && p<=0.90)}' && ok "24-bit at 150%: 0.600 -> $p" || bad "24-bit peak $p"
p="$(peak "$T/sfx/loud.wav")"; awk -v p="$p" 'BEGIN{exit !(p<=1.0 && p>0.95)}' && ok "the limiter keeps 0.95 x 150% under full scale: $p" || bad "limiter $p"
p="$(peak "$T/sfx/float.wav")"; [ "$p" = 0.450 ] && ok "float at 150%: 0.300 -> $p" || bad "float peak $p"
p="$(peak "$T/sfx/back.wav")"; awk -v p="$p" 'BEGIN{exit !(p>0.58 && p<0.62)}' && ok "8-bit mono at 150%: 0.400 -> $p" || bad "8-bit peak $p"
[ "$(md5 "$T/tools/backups/sounds/confirm.wav")" = "${o[confirm.wav]}" ] && ok "the original is kept" || bad "original"
cmp -s <(head -c 44 "$T/tools/backups/sounds/confirm.wav") <(head -c 44 "$T/sfx/confirm.wav") && ok "the header and other chunks are kept" || bad "header changed"

bash "$HERE/modules/sound-level/sound-level.sh" set 50 >/dev/null
p="$(peak "$T/sfx/confirm.wav")"; [ "$p" = 0.250 ] && ok "set 50 starts from the original, not the 150% copy: $p" || bad "50% peak $p"
cp "$T/sfx/loud.wav" "$T/sfx/new.wav.x"; python3 -c "
import sys; d=open(sys.argv[1],'rb').read(); open(sys.argv[2],'wb').write(d)" "$T/tools/backups/sounds/confirm.wav" "$T/sfx/confirm.wav"   # the user puts a different confirm sound in
printf 'x' >> "$T/sfx/confirm.wav"   # (changed, so it's new)
rm -f "$T/sfx/new.wav.x" "$T/sfx/back.wav"
bash "$HERE/modules/sound-level/sound-level.sh" refresh >/dev/null
[ "$(state=$(cat "$T/tools/state/sound-level.state"); echo "$state" | grep -c '^level=50$')" = 1 ] && ok "refresh keeps the level" || bad "refresh level"
[ -f "$T/tools/backups/sounds/back.wav" ] && bad "the original of a removed sound stays" || ok "the original of a removed sound is dropped"
[ "$(md5 "$T/tools/backups/sounds/confirm.wav")" != "${o[confirm.wav]}" ] && ok "a sound the user changed becomes the new original" || bad "changed sound"

bash "$HERE/modules/sound-level/sound-level.sh" restore >/dev/null
for n in move.WAV loud.wav float.wav jingle.mp3; do [ "$(md5 "$T/sfx/$n")" = "${o[$n]}" ] || bad "restore $n"; done
ok "restore puts the originals back byte for byte"
bash "$HERE/modules/sound-level/sound-level.sh" set 300 2>/dev/null && bad "300 accepted" || ok "levels outside 25-200 refused"
bash "$HERE/modules/sound-level/sound-level.sh" status | grep -q '^level=100$' && ok "status" || bad "status"
"$B" gain 150 "$T/sfx/jingle.mp3" "$T/x.wav" 2>/dev/null; [ $? = 2 ] && ok "cmsfx refuses a non-WAV with exit 2" || bad "non-wav rc"
exit $fail
