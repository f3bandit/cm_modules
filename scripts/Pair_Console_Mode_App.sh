#!/bin/bash
# Pair Console Mode App: shows the 6-digit code the PC or phone app asks for, big, with the SuperStation's address,
# until the app pairs (or 2 minutes go by). Run from Console Mode's Scripts menu, or by the PC app over SSH with
# --tv: then it puts itself on the TV (Linux's text screen 3, over Console Mode's) and switches back to Console
# Mode's screen when it's done.
# Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see LICENSE in cm_modules).
T=/media/fat/ConsoleMode/tools
R=$T/cmremote

if [ "${1:-}" = "--tv" ]; then
	# one at a time: an earlier one still showing goes first, and only the newest switches the TV back
	P=/tmp/cm_pair_tv.pid O=/tmp/cm_pair_tv.owner
	back=$(fgconsole 2>/dev/null || echo 2)
	[ "$back" = 3 ] && back=$(cat /tmp/cm_pair_tv.back 2>/dev/null || echo 2)
	echo "$back" > /tmp/cm_pair_tv.back
	echo $$ > $O
	[ -s $P ] && kill "$(cat $P)" 2>/dev/null
	openvt -c 3 -f -s -w -- /bin/bash -c 'echo $$ > /tmp/cm_pair_tv.pid; exec /bin/bash "$0" --on-tv' "$0"
	if [ "$(cat $O 2>/dev/null)" = "$$" ]; then
		chvt "$back"
		rm -f $P $O /tmp/cm_pair_tv.back
	fi
	exit 0
fi

# Console Mode's look: black, white text, quieter grey, the blue of its loading bar
W=$'\033[1;37m'; G=$'\033[0;37m'; D=$'\033[2;37m'; B=$'\033[1;34m'; BB=$'\033[44m'; K=$'\033[100m'; N=$'\033[0m'
cols=$(stty size 2>/dev/null | awk '{print $2}'); [ -z "$cols" ] || [ "$cols" -lt 40 ] && cols=80
rows=$(stty size 2>/dev/null | awk '{print $1}'); [ -z "$rows" ] || [ "$rows" -lt 20 ] && rows=30
# a line centred (its visible length given, colours aside)
ctr() { local pad=$(( (cols - $2) / 2 )); [ $pad -lt 0 ] && pad=0; printf '%*s%s\n' $pad '' "$1"; }
cls() { printf '\033[2J\033[H\033[?25l'; }
bye() { printf '\033[?25h%s' "$N"; }
trap bye EXIT

# big digits, 5 rows: row r of digit d is ROWS[r] at d*4 (3 wide), X a block
ROWS=(
"XXX   X XXX XXX X X XXX XXX XXX XXX XXX"
"X X   X   X   X X X X   X     X X X X X"
"X X   X XXX XXX XXX XXX XXX   X XXX XXX"
"X X   X X     X   X   X X X   X X X   X"
"XXX   X XXX XXX   X XXX XXX   X XXX XXX"
)
big() {
	local code=$1 r i line
	for r in 0 1 2 3 4; do
		line=""
		for i in 0 1 2 3 4 5; do
			line+="${ROWS[$r]:$(( ${code:$i:1} * 4 )):3}"
			[ $i = 2 ] && line+="     " || line+="  "
		done
		line=${line%  }
		ctr "$W${line//X/█}$N" ${#line}
	done
}
msg() { cls; local n=$(( rows / 2 - 3 )); for ((i = 0; i < n; i++)); do echo; done; ctr "${W}Pair Console Mode App${N}" 21; echo; ctr "$G$1$N" ${#1}; [ -n "${2:-}" ] && { echo; ctr "$D$2$N" ${#2}; }; }

if [ ! -x "$R" ]; then
	msg "The Console Mode tools aren't on this SuperStation yet." "Set it up from Console Mode for Windows (Connect)."
	sleep 10; exit 1
fi

# running, on, and started at every boot from now on
if ! pidof cmremote >/dev/null 2>&1; then
	start-stop-daemon -S -b -x "$R" -- serve >/dev/null 2>&1
	sleep 2
fi
"$R" boot on >/dev/null 2>&1
"$R" on >/dev/null 2>&1
"$R" pair >/dev/null 2>&1 || { msg "The app link didn't start." "Restart the SuperStation and try again."; sleep 8; exit 1; }

val() { printf '%s\n' "$INFO" | sed -n "s/^$1=//p" | head -1; }
total=120
end=$(( $(date +%s) + total + 5 ))
while [ "$(date +%s)" -lt "$end" ]; do
	INFO=$("$R" info 2>/dev/null)
	code=$(val code); left=$(val code_left); paired=$(val paired)
	addr=$(printf '%s\n' "$INFO" | sed -n 's/^address=//p' | sed 's/:8767$//' | head -1)
	if [ -n "$paired" ]; then
		msg "Paired with ${paired%% (*}" "Going back to Console Mode..."
		sleep 4; exit 0
	fi
	if [ -z "$code" ]; then
		msg "The code ran out." "Open Pair Console Mode App again for a new one."
		sleep 6; exit 0
	fi
	cls
	top=$(( (rows - 20) / 2 )); [ $top -lt 1 ] && top=1
	for ((i = 0; i < top; i++)); do echo; done
	ctr "${D}CONSOLE MODE${N}" 12
	ctr "${W}Pair a PC or phone${N}" 18
	echo; echo
	big "$code"
	echo; echo
	l1="In Console Mode for Windows or the phone app,"; ctr "$G$l1$N" ${#l1}
	l2="choose this SuperStation and type the code."; ctr "$G$l2$N" ${#l2}
	echo
	if [ -n "$addr" ]; then
		l3="SuperStation: $addr"; ctr "$B$l3$N" ${#l3}
	else
		l3="No network yet: connect WiFi or a cable first (Settings)."; ctr "$G$l3$N" ${#l3}
	fi
	echo; echo
	# the time left, as Console Mode's blue bar
	bw=40; f=$(( ${left:-0} * bw / total )); [ $f -gt $bw ] && f=$bw
	bar="$BB$(printf '%*s' $f '')$K$(printf '%*s' $(( bw - f )) '')$N"
	ctr "$bar" $bw
	l4="${left:-0} s left   -   any button: back"; ctr "$D$l4$N" ${#l4}
	if [ -t 0 ]; then
		if read -r -t 1 -n 1 _ 2>/dev/null; then "$R" pair_stop >/dev/null 2>&1; exit 0; fi
	else
		sleep 1
	fi
done
"$R" pair_stop >/dev/null 2>&1
exit 0
