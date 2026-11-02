// SS1 Link as an MCP server, so the Claude desktop app can use the MiSTer directly.
// Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
//
// MCP over stdio: JSON-RPC 2.0, one message per line. stdout carries only protocol messages;
// everything else goes to stderr.
package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

const maxText = 200_000

type rpcReq struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id,omitempty"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params,omitempty"`
}

type rpcErr struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

type rpcResp struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id"`
	Result  any             `json:"result,omitempty"`
	Error   *rpcErr         `json:"error,omitempty"`
}

type tool struct {
	Name        string         `json:"name"`
	Description string         `json:"description"`
	InputSchema map[string]any `json:"inputSchema"`
}

func obj(props map[string]any, req ...string) map[string]any {
	m := map[string]any{"type": "object", "properties": props}
	if len(req) > 0 {
		m["required"] = req
	}
	return m
}
func str(d string) map[string]any { return map[string]any{"type": "string", "description": d} }
func num(d string) map[string]any { return map[string]any{"type": "number", "description": d} }

var tools = []tool{
	{"status", "[SS1 Link / Console Mode] Connect to the MiSTer (SuperStation One dock debug port, or SSH) and report how it's reached, MiSTer Linux version, kernel, uptime, the running core and free space.", obj(map[string]any{})},
	{"list_ports", "[SS1 Link / Console Mode] List the serial ports on this PC (the dock's USB-C debug port shows up as a USB serial port).", obj(map[string]any{})},
	{"run", "[SS1 Link / Console Mode] Run a bash command or script on the MiSTer as root and return its output and exit code. Multi-line scripts are fine. Output over 200 KB is cut.", obj(map[string]any{
		"command":   str("Shell command or script"),
		"timeout_s": num("Seconds to wait (default 120)"),
	}, "command")},
	{"push_file", "[SS1 Link / Console Mode] Copy a file from this PC to the MiSTer (checked with md5).", obj(map[string]any{
		"local_path":  str("Path on this PC"),
		"remote_path": str("Full path on the MiSTer"),
		"mode":        str("chmod mode, default 644 (755 for scripts)"),
	}, "local_path", "remote_path")},
	{"push_text", "[SS1 Link / Console Mode] Write text to a file on the MiSTer (scripts, settings), creating folders as needed.", obj(map[string]any{
		"text":        str("File contents"),
		"remote_path": str("Full path on the MiSTer"),
		"mode":        str("chmod mode, default 644"),
	}, "text", "remote_path")},
	{"pull_file", "[SS1 Link / Console Mode] Copy a file from the MiSTer to this PC (checked with md5).", obj(map[string]any{
		"remote_path": str("Full path on the MiSTer"),
		"local_path":  str("Where to save it on this PC"),
	}, "remote_path", "local_path")},
	{"read_file", "[SS1 Link / Console Mode] Read a text file on the MiSTer and return it.", obj(map[string]any{
		"remote_path": str("Full path on the MiSTer"),
		"max_bytes":   num("Return at most this many bytes (default 200000)"),
	}, "remote_path")},
	{"serial_log", "[SS1 Link / Console Mode] Listen to the serial console without typing (MiSTer Main's messages, kernel messages) for some seconds, or until a piece of text appears. Serial only.", obj(map[string]any{
		"seconds": num("How long to listen (default 10, max 600)"),
		"until":   str("Stop early when this text appears"),
	})},
	{"recent_output", "[SS1 Link / Console Mode] Return the last part of everything the serial console printed since connecting, including Main's output. Serial only.", obj(map[string]any{
		"max_bytes": num("Default 20000"),
	})},
	{"reboot", "[SS1 Link / Console Mode] Reboot the MiSTer and return the full boot log (U-Boot, kernel, Main start), then log in again. Serial only. Nothing is typed until the login prompt, so the boot isn't interrupted.", obj(map[string]any{
		"timeout_s": num("Seconds to wait for the login prompt (default 180)"),
	})},
	{"press", "[SS1 Link / Console Mode] Press buttons in Console Mode from here: a virtual keyboard and pad on the SS1 (started the first time). Names in order, comma or space separated: up, down, left, right, a/ok (Enter), b/back (Esc), x (search), y (favourite), l, r, select, start, pgup, pgdn, f12, key:CODE or pad:CODE (Linux input codes). Take a screenshot afterwards to see the result.", obj(map[string]any{
		"buttons": str("e.g. \"down, down, a\""),
		"gap_ms":  num("Pause between presses in ms (default 350)"),
	}, "buttons")},
	{"screenshot", "[SS1 Link / Console Mode] What's on the TV: Console Mode's screen (the Linux framebuffer) as a PNG. While a core is on screen the framebuffer is black.", obj(map[string]any{
		"scale": num("1 full size (1280x720), 2 half (default), up to 4"),
	})},
	{"cm_state", "[SS1 Link / Console Mode] Console Mode's state: version, whether it's running, the core on screen, its saved state (.state), favourites and history (gamelist.ini), the update log, its app log, and ss1jobd's last job.", obj(map[string]any{
		"log_lines": num("App log lines (default 20)"),
	})},
	{"disconnect", "[SS1 Link / Console Mode] Close the connection (kernel messages and the terminal are put back the way they were). The next call reconnects.", obj(map[string]any{})},
}

type mcpServer struct {
	sess *session
	out  *json.Encoder
	wmu  sync.Mutex
}

func runMCP(cfg Config) error {
	log.SetOutput(os.Stderr)
	log.SetPrefix("ss1-link: ")
	srv := &mcpServer{sess: &session{cfg: cfg}, out: json.NewEncoder(os.Stdout)}
	defer srv.sess.close()
	srv.sess.keep()
	rd := bufio.NewReaderSize(os.Stdin, 1<<20)
	var wg sync.WaitGroup
	for {
		line, err := rd.ReadBytes('\n')
		if len(strings.TrimSpace(string(line))) > 0 {
			var req rpcReq
			if jerr := json.Unmarshal(line, &req); jerr != nil {
				srv.send(rpcResp{JSONRPC: "2.0", ID: json.RawMessage("null"), Error: &rpcErr{-32700, "parse error"}})
			} else {
				wg.Add(1)
				go func() { defer wg.Done(); srv.handle(req) }()
			}
		}
		if err == io.EOF {
			wg.Wait()
			return nil
		}
		if err != nil {
			return err
		}
	}
}

func (m *mcpServer) send(r rpcResp) {
	m.wmu.Lock()
	defer m.wmu.Unlock()
	_ = m.out.Encode(r)
}

func (m *mcpServer) handle(req rpcReq) {
	if len(req.ID) == 0 {
		return // a notification (initialized, cancelled): nothing to answer
	}
	reply := func(res any) { m.send(rpcResp{JSONRPC: "2.0", ID: req.ID, Result: res}) }
	fail := func(code int, msg string) {
		m.send(rpcResp{JSONRPC: "2.0", ID: req.ID, Error: &rpcErr{code, msg}})
	}
	switch req.Method {
	case "initialize":
		var p struct {
			ProtocolVersion string `json:"protocolVersion"`
		}
		_ = json.Unmarshal(req.Params, &p)
		if p.ProtocolVersion == "" {
			p.ProtocolVersion = "2025-06-18"
		}
		reply(map[string]any{
			"protocolVersion": p.ProtocolVersion,
			"capabilities":    map[string]any{"tools": map[string]any{}},
			"serverInfo":      map[string]any{"name": "ss1-link", "version": version},
			"instructions": "SS1 Link: for an SS1 running Console Mode (Phosphor Link, phosphor-link, is the one for Phosphor OS). Tools for a MiSTer FPGA (DE10-Nano, SuperStation One) reached through the SuperStation One dock's " +
				"USB-C debug port (serial console) or SSH. Commands run as root. The SD card is /media/fat. " +
				"Console Mode lives in /media/fat/ConsoleMode (its program ConsoleMode_arm, logs in Logs/, saved state in .state). " +
				"press drives it with a virtual keyboard and pad, screenshot shows the TV, cm_state sums it up.",
		})
	case "ping":
		reply(map[string]any{})
	case "tools/list":
		reply(map[string]any{"tools": tools})
	case "tools/call":
		var p struct {
			Name      string         `json:"name"`
			Arguments map[string]any `json:"arguments"`
		}
		if err := json.Unmarshal(req.Params, &p); err != nil {
			fail(-32602, "bad parameters")
			return
		}
		if p.Name == "screenshot" {
			l, err := m.sess.get()
			if err != nil {
				reply(map[string]any{"content": []map[string]any{{"type": "text", "text": "Can't reach the MiSTer: " + err.Error()}}, "isError": true})
				return
			}
			if p.Arguments == nil {
				p.Arguments = map[string]any{}
			}
			blocks, isErr := cmScreenshot(l, p.Arguments)
			reply(map[string]any{"content": blocks, "isError": isErr})
			return
		}
		text, isErr := m.call(p.Name, p.Arguments)
		reply(map[string]any{"content": []map[string]any{{"type": "text", "text": text}}, "isError": isErr})
	default:
		fail(-32601, "unknown method "+req.Method)
	}
}

// tools that do the same thing when repeated (not run, press or reboot)
var retryable = map[string]bool{"status": true, "push_file": true, "push_text": true, "pull_file": true, "read_file": true, "cm_state": true}

func firstLine(s string) string {
	s, _, _ = strings.Cut(s, "\n")
	return s
}

func argS(a map[string]any, k, def string) string {
	if v, ok := a[k].(string); ok && v != "" {
		return v
	}
	return def
}

func argN(a map[string]any, k string, def float64) float64 {
	if v, ok := a[k].(float64); ok && v > 0 {
		return v
	}
	return def
}

func secs(f float64) time.Duration { return time.Duration(f * float64(time.Second)) }

func (m *mcpServer) call(name string, a map[string]any) (string, bool) {
	if a == nil {
		a = map[string]any{}
	}
	if name == "list_ports" {
		ps, err := listPorts()
		if err != nil {
			return err.Error(), true
		}
		return formatPorts(ps), false
	}
	if name == "disconnect" {
		m.sess.drop()
		return "disconnected", false
	}
	l, err := m.sess.get()
	if err != nil {
		return "Can't reach the MiSTer: " + err.Error(), true
	}
	text, isErr, broken := m.callOn(l, name, a)
	if broken {
		m.sess.drop()
		// watchdog: the link died under us. Tools that are safe to repeat get one more go on a fresh link.
		if retryable[name] {
			log.Printf("watchdog: %s failed on %s (%s), reconnecting and trying again", name, l.Name(), firstLine(text))
			if l2, err := m.sess.get(); err == nil {
				text, isErr, broken = m.callOn(l2, name, a)
				if broken {
					m.sess.drop()
				}
			}
		} else {
			text += "\n[the link was reset; the next call reconnects]"
		}
	}
	return text, isErr
}

func (m *mcpServer) callOn(l Link, name string, a map[string]any) (text string, isErr, broken bool) {
	errOut := func(err error) (string, bool, bool) {
		// a timeout leaves the console usable; anything else (port gone, ssh dropped) reconnects next time
		b := !strings.Contains(err.Error(), "timed out")
		return err.Error(), true, b
	}
	switch name {
	case "status":
		out, _, err := l.Run(statusScript, 30*time.Second)
		if err != nil {
			return errOut(err)
		}
		return "connected by " + l.Name() + "\n" + out, false, false
	case "run":
		cmd := argS(a, "command", "")
		if cmd == "" {
			return "command is empty", true, false
		}
		out, rc, err := l.Run(cmd, secs(argN(a, "timeout_s", 120)))
		if err != nil {
			t, _, b := errOut(err)
			return clip(out, maxText) + "\n[" + t + "]", true, b
		}
		return fmt.Sprintf("%s\n[exit %d]", clip(out, maxText), rc), rc != 0, false
	case "push_file":
		lp := argS(a, "local_path", "")
		data, err := os.ReadFile(lp)
		if err != nil {
			return err.Error(), true, false
		}
		rp := argS(a, "remote_path", "")
		if err := l.Push(data, rp, argS(a, "mode", "644")); err != nil {
			return errOut(err)
		}
		return fmt.Sprintf("copied %s to %s (%d bytes, md5 checked)", lp, rp, len(data)), false, false
	case "push_text":
		rp := argS(a, "remote_path", "")
		t, _ := a["text"].(string)
		if err := l.Push([]byte(t), rp, argS(a, "mode", "644")); err != nil {
			return errOut(err)
		}
		return fmt.Sprintf("wrote %s (%d bytes)", rp, len(t)), false, false
	case "pull_file":
		rp, lp := argS(a, "remote_path", ""), argS(a, "local_path", "")
		data, err := l.Pull(rp)
		if err != nil {
			return errOut(err)
		}
		if dir := filepath.Dir(lp); dir != "" {
			_ = os.MkdirAll(dir, 0755)
		}
		if err := os.WriteFile(lp, data, 0644); err != nil {
			return err.Error(), true, false
		}
		return fmt.Sprintf("saved %s to %s (%d bytes, md5 checked)", rp, lp, len(data)), false, false
	case "read_file":
		data, err := l.Pull(argS(a, "remote_path", ""))
		if err != nil {
			return errOut(err)
		}
		mx := int(argN(a, "max_bytes", maxText))
		if !utf8.Valid(data) {
			return fmt.Sprintf("%d bytes of binary data; use pull_file to save it", len(data)), false, false
		}
		return clip(string(data), mx), false, false
	case "serial_log":
		li, err := m.sess.console()
		if err != nil {
			return err.Error(), true, false
		}
		d := argN(a, "seconds", 10)
		if d > 600 {
			d = 600
		}
		out, err := li.Listen(secs(d), argS(a, "until", ""))
		if err != nil {
			return errOut(err)
		}
		if strings.TrimSpace(out) == "" {
			return "(nothing printed)", false, false
		}
		return clip(out, maxText), false, false
	case "recent_output":
		li, err := m.sess.console()
		if err != nil {
			return err.Error(), true, false
		}
		return li.Recent(int(argN(a, "max_bytes", 20000))), false, false
	case "press":
		return cmPress(l, a)
	case "cm_state":
		return cmState(l, a)
	case "reboot":
		rb, err := m.sess.console()
		if err != nil {
			// no debug port: reboot over SSH instead (no boot log), and wait until it answers again
			if _, ok := l.(*SSHLink); !ok {
				return err.Error(), true, false
			}
			return m.sshReboot(l, secs(argN(a, "timeout_s", 180)), err)
		}
		out, err := rb.Reboot(secs(argN(a, "timeout_s", 180)))
		// an SSH link didn't survive the reboot either: reconnect both next time
		m.sess.drop()
		if err != nil {
			m.sess.dropConsole()
			return clip(out, maxText) + "\n[" + err.Error() + "]", true, false
		}
		return clip(out, maxText), false, false
	}
	return "unknown tool " + name, true, false
}

const statusScript = `echo "MiSTer Linux: $(cat /MiSTer.version 2>/dev/null || echo unknown)"
echo "kernel: $(uname -r)"
echo "uptime: $(uptime)"
echo "core: $(cat /tmp/CORENAME 2>/dev/null || echo none)"
m=""; for e in /proc/[0-9]*/exe; do t=$(readlink "$e" 2>/dev/null); case "$t" in /media/fat/MiSTer*|/media/fat/*/MiSTer*) m="$t"; break;; esac; done
echo "Main: ${m:-not running}"
df -h /media/fat 2>/dev/null | tail -n 1 | awk '{ print "SD card: " $4 " free of " $2 }'
ip -4 -o addr show 2>/dev/null | awk '$2 != "lo" { print "address: " $2 " " $4 }'
[ -x /media/fat/ConsoleMode/ConsoleMode_arm ] && echo "Console Mode: $(cat /media/fat/ConsoleMode/.cm_latest_seen 2>/dev/null || echo installed)" || echo "Console Mode: not installed"
`

func formatPorts(ps []PortInfo) string {
	if len(ps) == 0 {
		return "no serial ports found"
	}
	var b strings.Builder
	for _, p := range ps {
		fmt.Fprintf(&b, "%s", p.Name)
		if p.USB {
			fmt.Fprintf(&b, "  USB %s:%s", p.VID, p.PID)
			if p.Product != "" {
				fmt.Fprintf(&b, "  %s", p.Product)
			}
			if p.Serial != "" {
				fmt.Fprintf(&b, "  serial %s", p.Serial)
			}
		}
		b.WriteString("\n")
	}
	return b.String()
}

// sshReboot: the reboot tool when the debug port isn't answering. Reboots over SSH and waits for SSH to
// answer again, so the next call doesn't land on a MiSTer that's half-way through booting.
func (m *mcpServer) sshReboot(l Link, timeout time.Duration, why error) (string, bool, bool) {
	_, _, _ = l.Run(safeReboot, 35*time.Second) // the connection drops as it goes down
	m.sess.drop()
	time.Sleep(20 * time.Second)
	end := time.Now().Add(timeout)
	for time.Now().Before(end) {
		if l2, err := m.sess.get(); err == nil {
			if out, _, err := l2.Run("cat /proc/uptime", 10*time.Second); err == nil {
				return "rebooted over SSH (no boot log: " + why.Error() + ")\nup again, uptime " + strings.Fields(out + " ?")[0] + " s", false, false
			}
			m.sess.drop()
		}
		time.Sleep(5 * time.Second)
	}
	return fmt.Sprintf("rebooted over SSH, but it didn't answer again within %s", timeout), true, false
}
