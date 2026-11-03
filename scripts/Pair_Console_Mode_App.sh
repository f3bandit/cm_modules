#!/bin/bash
# Pair Console Mode App: shows the 6-digit code the PC or phone app asks for, and the SuperStation's address,
# until the app pairs (or 2 minutes go by). Run from Console Mode's Scripts menu.
# Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see LICENSE in cm_modules).
T=/media/fat/ConsoleMode/tools
R=$T/cmremote

if [ ! -x "$R" ]; then
	echo ""
	echo "  The Console Mode tools aren't installed on this SuperStation."
	echo "  Copy Install_Console_Mode_Tools.sh to the SD card's Scripts folder"
	echo "  and run it from Scripts first."
	sleep 10
	exit 1
fi

# running, on, and started at every boot from now on
if ! pidof cmremote >/dev/null 2>&1; then
	start-stop-daemon -S -b -x "$R" -- serve >/dev/null 2>&1
	sleep 2
fi
"$R" boot on >/dev/null 2>&1
"$R" on >/dev/null 2>&1
"$R" pair >/dev/null 2>&1 || { echo "  The app link didn't start. Restart the SuperStation and try again."; sleep 8; exit 1; }

val() { printf '%s\n' "$INFO" | sed -n "s/^$1=//p" | head -1; }
end=$(( $(date +%s) + 125 ))
while [ "$(date +%s)" -lt "$end" ]; do
	INFO=$("$R" info 2>/dev/null)
	code=$(val code); left=$(val code_left); paired=$(val paired)
	addrs=$(printf '%s\n' "$INFO" | sed -n 's/^address=//p' | sed 's/:8767$//' | tr '\n' ' ')
	clear
	echo ""
	echo "   PAIR CONSOLE MODE APP"
	echo "   ====================="
	echo ""
	if [ -n "$paired" ]; then
		echo "   Paired with: $paired"
		echo ""
		echo "   Done. Going back to Console Mode..."
		sleep 4
		exit 0
	fi
	if [ -z "$code" ]; then
		echo "   The code ran out. Run Pair Console Mode App again for a new one."
		sleep 6
		exit 0
	fi
	echo "   In Console Mode for Windows (or the phone app), choose this"
	echo "   SuperStation and type this code:"
	echo ""
	echo "          ${code:0:1}  ${code:1:1}  ${code:2:1}   ${code:3:1}  ${code:4:1}  ${code:5:1}"
	echo ""
	if [ -n "$addrs" ]; then
		echo "   SuperStation address: $addrs"
	else
		echo "   No network: connect WiFi or a cable first (Console Mode's settings)."
	fi
	echo ""
	echo "   The code works for ${left:-0} more seconds."
	echo "   Press any button to go back without pairing."
	if [ -t 0 ]; then
		if read -r -t 1 -n 1 _ 2>/dev/null; then "$R" pair_stop >/dev/null 2>&1; exit 0; fi
	else
		sleep 1
	fi
done
"$R" pair_stop >/dev/null 2>&1
exit 0
