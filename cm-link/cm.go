// CM Link's Console Mode tools: press buttons (a virtual keyboard and pad on the SuperStation), take screenshots
// (the Linux framebuffer Console Mode draws on), and Console Mode's state.
// Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md).
package main

import (
	_ "embed"
	"encoding/base64"
	"fmt"
	"strings"
	"time"
)

// cmkbd: a virtual USB keyboard + gamepad for MiSTer (uinput), driven over a fifo.
//
//go:embed helper/cmkbd_arm
var kbdHelper []byte

const kbdDir = "/tmp/cmlink"

// what each button sends: a key on the virtual keyboard, or a button on the virtual pad. Console Mode
// takes the arrows, Enter (OK) and Esc (Back) from a keyboard; X (search), Y (favourite), L, R, Select and
// Start from a controller.
var cmButtons = map[string]string{
	"up": "t 103", "down": "t 108", "left": "t 105", "right": "t 106",
	"a": "t 28", "ok": "t 28", "enter": "t 28", "b": "t 1", "back": "t 1", "esc": "t 1",
	"x": "pt 307", "y": "pt 308", "l": "pt 310", "r": "pt 311", "select": "pt 314", "start": "pt 315",
	"pgup": "t 104", "pgdn": "t 109", "f12": "t 88",
}

// ensureKbd puts the helper on the SuperStation and starts it (once). A fresh start waits a moment: Console Mode
// looks for new input devices every few seconds.
func ensureKbd(l Link) (string, error) {
	sum := md5hex(kbdHelper)
	out, _, err := l.Run(fmt.Sprintf(`mkdir -p %[1]s; [ "$(md5sum %[1]s/cmkbd 2>/dev/null | cut -d' ' -f1)" = %[2]s ] && echo have`, kbdDir, sum), 20*time.Second)
	if err != nil {
		return "", err
	}
	if !strings.Contains(out, "have") {
		if err := l.Push(kbdHelper, kbdDir+"/cmkbd", "755"); err != nil {
			return "", err
		}
	}
	// the whole pipeline detached (setsid, no inherited stdout/stderr), or the command never returns
	out, _, err = l.Run(fmt.Sprintf(`cd %[1]s; [ -p kin ] || mkfifo kin
if pidof cmkbd > /dev/null; then echo running; else
  setsid sh -c 'while :; do cat %[1]s/kin; done | %[1]s/cmkbd > %[1]s/kbd.log 2>&1' < /dev/null > /dev/null 2>&1 &
  sleep 4; echo started; fi`, kbdDir), 30*time.Second)
	return strings.TrimSpace(out), err
}

func cmPress(l Link, a map[string]any) (string, bool, bool) {
	var cmds, names []string
	for _, f := range strings.FieldsFunc(strings.ToLower(argS(a, "buttons", "")), func(r rune) bool { return r == ',' || r == ' ' || r == '\n' || r == '\t' }) {
		c, ok := cmButtons[f]
		switch {
		case ok:
		case strings.HasPrefix(f, "key:"):
			c = "t " + strings.TrimPrefix(f, "key:")
		case strings.HasPrefix(f, "pad:"):
			c = "pt " + strings.TrimPrefix(f, "pad:")
		default:
			return "unknown button " + f + " (up, down, left, right, a/ok, b/back, x, y, l, r, select, start, pgup, pgdn, f12, key:CODE, pad:CODE)", true, false
		}
		cmds, names = append(cmds, c), append(names, f)
	}
	if len(cmds) == 0 || len(cmds) > 60 {
		return "give 1 to 60 buttons", true, false
	}
	gap := argN(a, "gap_ms", 350)
	if gap < 50 {
		gap = 50
	}
	if gap > 3000 {
		gap = 3000
	}
	st, err := ensureKbd(l)
	if err != nil {
		return err.Error(), true, true
	}
	var b strings.Builder
	for _, c := range cmds {
		fmt.Fprintf(&b, "echo '%s' > %s/kin; sleep %.3f\n", c, kbdDir, gap/1000)
	}
	if _, _, err := l.Run(b.String(), secs(20+float64(len(cmds))*gap/1000)); err != nil {
		return err.Error(), true, !strings.Contains(err.Error(), "timed out")
	}
	return fmt.Sprintf("pressed %s (virtual keyboard/pad %s)", strings.Join(names, ", "), st), false, false
}

// the framebuffer as PNG: Console Mode draws on /dev/fb0 (32-bit BGRA); a core on screen leaves it black
const shotScript = `python3 - %d <<'EOF'
import sys, zlib, struct, base64
s = max(1, int(sys.argv[1]))
rd = lambda p: open('/sys/class/graphics/fb0/' + p).read().strip()
w, h = map(int, rd('virtual_size').split(','))
bpp, stride = int(rd('bits_per_pixel')), int(rd('stride'))
raw = open('/dev/fb0', 'rb').read(stride * h)
rows = bytearray()
for y in range(0, h, s):
    r = raw[y * stride: y * stride + w * 4]
    q = bytearray((w // s) * 3)
    q[0::3] = r[2::4 * s][:w // s]; q[1::3] = r[1::4 * s][:w // s]; q[2::3] = r[0::4 * s][:w // s]
    rows.append(0); rows += q
c = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + c(b'IHDR', struct.pack('>IIBBBBB', w // s, (h + s - 1) // s, 8, 2, 0, 0, 0)) + c(b'IDAT', zlib.compress(bytes(rows), 9)) + c(b'IEND', b'')
sys.stdout.write(base64.b64encode(png).decode())
EOF`

// cmScreenshot returns MCP content blocks: the image, and a line on what's on screen
func cmScreenshot(l Link, a map[string]any) ([]map[string]any, bool) {
	scale := int(argN(a, "scale", 2))
	if scale > 4 {
		scale = 4
	}
	out, rc, err := l.Run(fmt.Sprintf(shotScript, scale)+"\necho; echo \"core=$(cat /tmp/CORENAME 2>/dev/null)\"", 60*time.Second)
	if err != nil || rc != 0 {
		msg := "screenshot failed"
		if err != nil {
			msg += ": " + err.Error()
		} else {
			msg += ": " + clip(out, 2000)
		}
		return []map[string]any{{"type": "text", "text": msg}}, true
	}
	lines := strings.Split(strings.TrimSpace(out), "\n")
	b64 := strings.TrimSpace(lines[0])
	if _, err := base64.StdEncoding.DecodeString(b64); err != nil {
		return []map[string]any{{"type": "text", "text": "screenshot: bad data: " + clip(out, 500)}}, true
	}
	core := ""
	if len(lines) > 1 {
		core = strings.TrimPrefix(lines[len(lines)-1], "core=")
	}
	note := "Console Mode's screen (the Linux framebuffer)"
	if core != "" && !strings.EqualFold(core, "MENU") {
		note = "a core is on screen (" + core + "): the framebuffer only shows Console Mode or MiSTer's menu, so this may be black"
	}
	return []map[string]any{{"type": "image", "data": b64, "mimeType": "image/png"}, {"type": "text", "text": note}}, false
}

const cmStateScript = `cm=/media/fat/ConsoleMode
echo "Console Mode: $(cat $cm/.cm_latest_seen 2>/dev/null || echo '?') ($( [ -x $cm/ConsoleMode_arm ] && echo installed || echo 'not installed'))"
echo "core on screen: $(cat /tmp/CORENAME 2>/dev/null || echo '?')"
p=$(pidof ConsoleMode_arm); echo "ConsoleMode_arm: ${p:-not running}"
echo "--- .state"; cat $cm/.state 2>/dev/null | tr -d '\n' | head -c 1500; echo
echo "--- gamelist.ini"; head -c 1500 $cm/gamelist.ini 2>/dev/null; echo
echo "--- update.log (last 5)"; tail -n 5 $cm/Logs/update.log 2>/dev/null
echo "--- app log (last %[1]d, without the video-mode noise)"; grep -v "Video mode 1920x1080" $cm/Logs/app/consolemode_app.log 2>/dev/null | tail -n %[1]d
[ -f $cm/tools/last_job.txt ] && { echo "--- cmjobd: last job"; cat $cm/tools/last_job.txt; }`

func cmState(l Link, a map[string]any) (string, bool, bool) {
	out, _, err := l.Run(fmt.Sprintf(cmStateScript, int(argN(a, "log_lines", 20))), 30*time.Second)
	if err != nil {
		return err.Error(), true, !strings.Contains(err.Error(), "timed out")
	}
	return clip(out, maxText), false, false
}
