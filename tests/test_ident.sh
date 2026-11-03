#!/bin/sh
# cmident: a cartridge's CRC32 with the copier header left out, the way No-Intro counts it
set -e
B="$1"; T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
printf 'hello' > "$T/a.gb"
# NES: a 16-byte iNES header, then the data "hello" (crc32 3610a686)
printf 'NES\032\001\001\000\000\000\000\000\000\000\000\000\000hello' > "$T/b.nes"
out=$(printf 'cart\t%s\ncart\t%s\nsnes\t%s\n' "$T/a.gb" "$T/b.nes" "$T/missing.sfc" | "$B")
echo "$out" | grep -q "a.gb	3610a686		5" || { echo "FAIL gb: $out"; exit 1; }
echo "$out" | grep -q "b.nes	3610a686		" || { echo "FAIL nes: $out"; exit 1; }
echo "$out" | grep -q "missing.sfc			-1" || { echo "FAIL missing: $out"; exit 1; }
echo "ident: all passed"
