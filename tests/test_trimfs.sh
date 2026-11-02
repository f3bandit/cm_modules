#!/bin/sh
# cmtrimfs on a fresh exFAT image (needs mkfs.exfat and fsck.exfat; skipped without them).
# Copyright (C) 2026 f3bandit. GPL-3.0-or-later.
B=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
command -v mkfs.exfat >/dev/null && command -v fsck.exfat >/dev/null || { echo "trimfs: skipped (no exfatprogs)"; exit 0; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
truncate -s 16M "$T/x.img" && mkfs.exfat -c 4096 "$T/x.img" >/dev/null 2>&1 || { echo "FAIL mkfs"; exit 1; }
head -c 65536 "$T/x.img" | md5sum > "$T/head.md5"
out=$("$B" -i "$T/x.img") || { echo "FAIL run: $out"; exit 1; }
free=$(echo "$out" | sed -n 's/^free=//p'); tr=$(echo "$out" | sed -n 's/^trimmed=//p')
[ -n "$free" ] && [ "$free" = "$tr" ] && [ "$free" -gt 14000000 ] || { echo "FAIL trimmed $tr of $free"; exit 1; }
fsck.exfat -n "$T/x.img" 2>&1 | grep -q clean || { echo "FAIL fsck"; exit 1; }
head -c 65536 "$T/x.img" | md5sum | cmp -s - "$T/head.md5" || { echo "FAIL metadata changed"; exit 1; }
"$B" -i /dev/null >/dev/null 2>&1 && { echo "FAIL not exfat accepted"; exit 1; }
echo "trimfs: all passed"
