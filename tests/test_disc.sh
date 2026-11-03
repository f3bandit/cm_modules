#!/bin/sh
# cmdisc against a made-up raw image (a PlayStation data track with SYSTEM.CNF, and an audio track):
# what it says the disc is, and the pretend files read back the same, in order and at random, from
# several readers at once (the read-ahead buffer). The folder part is skipped without FUSE.
set -e
B="$1"; T=$(mktemp -d); trap 'umount -l "$T/m" 2>/dev/null || true; rm -rf "$T"' EXIT
python3 - "$T" <<'PY'
import sys, random
d = sys.argv[1]
def sec(lba, user):
    s = bytearray(2352); s[1:11] = b'\xff' * 10
    m, r = divmod(lba + 150, 4500); sc, f = divmod(r, 75); b = lambda x: ((x // 10) << 4) | (x % 10)
    s[12:15] = bytes([b(m), b(sc), b(f)]); s[15] = 2; s[24:24 + len(user)] = user; return bytes(s)
data = bytearray()
for i in range(600):
    u = bytearray(2048)
    if i == 16:   # the volume descriptor: system "PLAYSTATION", the root directory at sector 22
        u[0] = 1; u[1:6] = b'CD001'; u[8:19] = b'PLAYSTATION'
        u[156] = 34; u[158:162] = (22).to_bytes(4, 'little'); u[166:170] = (2048).to_bytes(4, 'little')
    elif i == 22:  # the root: SYSTEM.CNF at sector 23
        n = b'SYSTEM.CNF;1'; r = bytearray(33 + len(n) + 1); r[0] = len(r); r[2:6] = (23).to_bytes(4, 'little')
        r[10:14] = (60).to_bytes(4, 'little'); r[32] = len(n); r[33:33 + len(n)] = n; u[:len(r)] = r
    elif i == 23:
        u[:40] = b'BOOT = cdrom:\\SLUS_012.34;1\r\nTCB = 4\r\n'
    else:
        random.seed(i); u[:] = bytes(random.getrandbits(8) for _ in range(2048))
    data += sec(i, u)
open(d + '/G (Track 1).bin', 'wb').write(data)
random.seed(7); open(d + '/G (Track 2).bin', 'wb').write(bytes(random.getrandbits(8) for _ in range(2352 * 900)))
open(d + '/G.cue', 'w').write('FILE "G (Track 1).bin" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n'
    'FILE "G (Track 2).bin" BINARY\r\n  TRACK 02 AUDIO\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:02:00\r\n')
open(d + '/C.cue', 'w').write('FILE "G (Track 1).bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n')
PY
out=$("$B" status --image "$T/G.cue" "$T/m")
for want in type=psx serial=SLUS-01234 core=PlayStation tracks=2 "track1=mode2 0 600" "track2=audio 750 900"; do
	echo "$out" | grep -qx "$want" || { echo "FAIL status: no $want in: $out"; exit 1; }
done
"$B" status --image "$T/C.cue" | grep -q "only raw" || { echo "FAIL: a 2048-byte image wasn't refused"; exit 1; }
if [ ! -c /dev/fuse ] || ! "$B" mount -b "$T/m" --image "$T/G.cue" --name "Test" >/dev/null 2>&1; then echo "disc: all passed (no FUSE here: the folder part skipped)"; exit 0; fi
grep -q 'TRACK 02 AUDIO' "$T/m/Test.cue" && grep -q 'INDEX 01 00:02:00' "$T/m/Test.cue" || { echo "FAIL cue: $(cat "$T/m/Test.cue")"; exit 1; }
cmp "$T/m/Test (Track 1).bin" "$T/G (Track 1).bin" && cmp "$T/m/Test (Track 2).bin" "$T/G (Track 2).bin" || { echo "FAIL: the files differ"; exit 1; }
python3 - "$T/m/Test (Track 2).bin" "$T/G (Track 2).bin" "$T/m/Test (Track 1).bin" "$T/G (Track 1).bin" <<'PY'
import sys, random, threading
bad = []
def reader(seed, a, b):
    r = random.Random(seed); fa = open(a, 'rb'); fb = open(b, 'rb'); size = fb.seek(0, 2)
    for _ in range(300):
        o = r.randrange(size); n = r.randrange(1, 300000)
        fa.seek(o); fb.seek(o)
        if fa.read(n) != fb.read(n): bad.append((a, o, n))
ts = [threading.Thread(target=reader, args=(s, sys.argv[1 + 2 * (s % 2)], sys.argv[2 + 2 * (s % 2)])) for s in range(6)]
[t.start() for t in ts]; [t.join() for t in ts]
if bad: print("FAIL random reads:", bad[:3]); sys.exit(1)
PY
"$B" status --image "$T/G.cue" "$T/m" | grep -qx playing=1 || { echo "FAIL: not seen as mounted"; exit 1; }
"$B" stop "$T/m" >/dev/null
sleep 1
grep -q " $T/m " /proc/mounts && { echo "FAIL: still mounted"; exit 1; }
echo "disc: all passed"
