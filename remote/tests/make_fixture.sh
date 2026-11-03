#!/bin/sh
# A pretend SD card and USB drive with Console Mode's layout, for test_remote.sh.
# Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../../LICENSE).
set -e
R=$1
rm -rf "$R"; mkdir -p "$R"
S=$R/fat/ConsoleMode/themeconfig/section_groups
mkdir -p "$S" "$R/fat/ConsoleMode/playtime"
cat > "$S/Handheld.ini" <<'X'
[CONSOLES]
consoleList = GAMEBOY
[GAMEBOY]
execs = none
romExts = .zip,.gb
romDirs = /media/fat/games/GAMEBOY/
X
cat > "$S/Console.ini" <<'X'
[CONSOLES]
consoleList = PLAYSTATION
[PLAYSTATION]
execs = none
romExts = .cue,.chd,.exe
romDirs = /media/fat/games/PSX/
X
G=$R/usb0/games/GAMEBOY
mkdir -p "$G/media" "$G/Europe" "$G/Cursor Test/media"
: > "$G/Tetris.gb"; : > "$G/Zelda & Link.gb"; : > "$G/notes.txt"; : > "$G/Europe/Super Mario Land (World).gb"
: > "$G/Cursor Test/A.gb"; : > "$G/Cursor Test/B.gb"
printf '\x89PNG\r\n\x1a\nfake' > "$G/media/Tetris.png"
cat > "$G/gamelist.xml" <<'X'
<?xml version='1.0' encoding='utf-8'?>
<gameList>
  <game id="1" source="x">
    <path>./Tetris.gb</path>
    <name>Tetris</name>
    <desc>Falling blocks &amp; lines.</desc>
    <releasedate>19890614T000000</releasedate>
    <developer>Nintendo</developer>
  </game>
</gameList>
X
P=$R/usb0/games/PSX
mkdir -p "$P/Castlevania (USA)" "$P/FF7" "$P/media"
: > "$P/Castlevania (USA)/game.cue"; : > "$P/Castlevania (USA)/game.bin"
: > "$P/FF7/FF7 (Disc 1).cue"; : > "$P/FF7/FF7 (Disc 2).cue"; : > "$P/FF7/FF7 (Disc 1).bin"
mkdir -p "$R/fat/games/GAMEBOY"; : > "$R/fat/games/GAMEBOY/On SD.gb"
printf 'FAVORITES=/media/usb0/games/GAMEBOY/Tetris.gb\n[HISTORY]\nPATH0=x\n' > "$R/fat/ConsoleMode/gamelist.ini"
printf '#v=1\n1793599986\t31\tGAMEBOY\t%s\n' "$R/usb0/games/GAMEBOY/Tetris.gb" > "$R/fat/ConsoleMode/playtime/history.tsv"
# arcade: Console Mode's art for these is /media/fat/media/<the .mra's name>.png
mkdir -p "$R/fat/_Arcade/_alternatives/_Pac" "$R/fat/media"
: > "$R/fat/_Arcade/_alternatives/_Pac/Pac-Man (Midway).mra"; : > "$R/fat/_Arcade/Galaga.mra"
printf '\x89PNG\r\n\x1a\nfake' > "$R/fat/media/Pac-Man (Midway).png"
# Update All's Game Artwork databases: docs/<system>/Artwork/<No-Intro name>.jpg (arcade: the set name)
mkdir -p "$R/usb0/docs/GAMEBOY/Artwork" "$R/fat/docs/Arcade/Artwork" "$R/fat/docs/GAMEBOY/Titles"
printf '<misterromdescription><name>Galaga</name><setname>galaga</setname></misterromdescription>' > "$R/fat/_Arcade/Galaga.mra"
