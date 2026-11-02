// SS1 Link: a PC program for reaching a MiSTer through the SuperStation One dock's
// USB-C debug port (or SSH), for people and for the Claude desktop app.
// Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
package main

import (
	"bytes"
	"crypto/rand"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"
)

const version = "0.1.0"

const usage = `SS1 Link ` + version + `: reach a MiSTer through the SuperStation One dock's debug port, or SSH.

  ss1-link ports                      list serial ports on this PC
  ss1-link status                     connect and show the MiSTer's state
  ss1-link run "<command>"            run a command on the MiSTer (exit code is passed back)
  ss1-link push <local> <remote> [mode]
  ss1-link pull <remote> <local>
  ss1-link watch [seconds]            show what the console prints (Main, kernel) without typing
  ss1-link reboot                     reboot and show the whole boot log
  ss1-link term                       interactive console (Ctrl-] quits)
  ss1-link selftest                   check the link end to end (commands, files both ways)
  ss1-link install-claude             add SS1 Link to the Claude desktop app
  ss1-link uninstall-claude           remove it again
  ss1-link mcp                        the Claude desktop app runs this; not for typing

Settings (all optional) go in ss1-link.ini next to this program: transport (auto, serial, ssh),
port (auto or COM5), baud, user, password, host (for SSH), ssh_user, ssh_password, log.
`

func die(err error) {
	fmt.Fprintln(os.Stderr, "error:", err)
	os.Exit(1)
}

func main() {
	cfg := loadConfig()
	if len(os.Args) < 2 {
		fmt.Print(usage)
		return
	}
	cmd, args := os.Args[1], os.Args[2:]
	switch cmd {
	case "help", "-h", "--help":
		fmt.Print(usage)
	case "version", "--version":
		fmt.Println(version)
	case "ports":
		ps, err := listPorts()
		if err != nil {
			die(err)
		}
		fmt.Print(formatPorts(ps))
	case "mcp":
		if err := runMCP(cfg); err != nil {
			die(err)
		}
	case "install-claude":
		if err := installClaude(); err != nil {
			die(err)
		}
	case "uninstall-claude":
		if err := uninstallClaude(); err != nil {
			die(err)
		}
	case "term":
		if err := runTerm(cfg); err != nil {
			die(err)
		}
	default:
		os.Exit(withLink(cfg, cmd, args))
	}
}

func withLink(cfg Config, cmd string, args []string) int {
	need := map[string]int{"status": 0, "run": 1, "push": 2, "pull": 2, "watch": 0, "reboot": 0, "selftest": 0}
	n, ok := need[cmd]
	if !ok {
		fmt.Fprint(os.Stderr, "unknown command "+cmd+"\n\n"+usage)
		return 2
	}
	if len(args) < n {
		fmt.Fprint(os.Stderr, usage)
		return 2
	}
	if (cmd == "watch" || cmd == "reboot") && strings.EqualFold(cfg.Transport, "auto") {
		cfg.Transport = "serial" // only the debug port shows the console
	}
	l, err := connect(cfg)
	if err != nil {
		die(err)
	}
	defer l.Close()
	fmt.Fprintln(os.Stderr, "connected by", l.Name())
	switch cmd {
	case "status":
		out, _, err := l.Run(statusScript, 30*time.Second)
		fmt.Print(out)
		if err != nil {
			die(err)
		}
	case "run":
		out, rc, err := l.Run(strings.Join(args, " "), 10*time.Minute)
		fmt.Print(out)
		if err != nil {
			die(err)
		}
		return rc
	case "push":
		data, err := os.ReadFile(args[0])
		if err != nil {
			die(err)
		}
		mode := "644"
		if len(args) > 2 {
			mode = args[2]
		}
		if err := l.Push(data, args[1], mode); err != nil {
			die(err)
		}
		fmt.Printf("copied %d bytes, md5 checked\n", len(data))
	case "pull":
		data, err := l.Pull(args[0])
		if err != nil {
			die(err)
		}
		if err := os.WriteFile(args[1], data, 0644); err != nil {
			die(err)
		}
		fmt.Printf("saved %d bytes, md5 checked\n", len(data))
	case "watch":
		li, ok := l.(Listener)
		if !ok {
			die(errNotSerial)
		}
		d := 30
		if len(args) > 0 {
			d, _ = strconv.Atoi(args[0])
		}
		out, err := li.Listen(time.Duration(d)*time.Second, "")
		fmt.Print(out)
		if err != nil {
			die(err)
		}
	case "reboot":
		rb, ok := l.(Rebooter)
		if !ok {
			die(errNotSerial)
		}
		out, err := rb.Reboot(3 * time.Minute)
		fmt.Print(out)
		if err != nil {
			die(err)
		}
	case "selftest":
		return selftest(l)
	}
	return 0
}

func selftest(l Link) int {
	fails := 0
	check := func(name string, err error) {
		if err != nil {
			fails++
			fmt.Printf("FAIL  %s: %v\n", name, err)
		} else {
			fmt.Printf("ok    %s\n", name)
		}
	}
	out, rc, err := l.Run("echo hello; echo to-stderr >&2; exit 3", 30*time.Second)
	if err == nil && (rc != 3 || out != "hello\nto-stderr\n") {
		err = fmt.Errorf("got rc %d, output %q", rc, out)
	}
	check("run (output, stderr, exit code)", err)

	quote := "a'b\"c $HOME `x` \\ end"
	out, _, err = l.Run("printf '%s\\n' "+sq(quote), 30*time.Second)
	if err == nil && out != quote+"\n" {
		err = fmt.Errorf("got %q", out)
	}
	check("run (quoting)", err)

	data := make([]byte, 96*1024)
	_, _ = rand.Read(data)
	remote := "/tmp/ss1-link-selftest.bin"
	start := time.Now()
	err = l.Push(data, remote, "600")
	check(fmt.Sprintf("push 96 KB (%.1fs)", time.Since(start).Seconds()), err)
	if err == nil {
		start = time.Now()
		back, err := l.Pull(remote)
		if err == nil && !bytes.Equal(back, data) {
			err = fmt.Errorf("data differs")
		}
		check(fmt.Sprintf("pull 96 KB (%.1fs)", time.Since(start).Seconds()), err)
	}
	_, _, err = l.Run("rm -f "+remote, 30*time.Second)
	check("clean up", err)

	big := strings.Repeat("0123456789abcdef", 4096)
	out, _, err = l.Run("for i in $(seq 1 4096); do printf 0123456789abcdef; done", 120*time.Second)
	if err == nil && out != big {
		err = fmt.Errorf("got %d bytes", len(out))
	}
	check("64 KB of output", err)

	if fails > 0 {
		fmt.Printf("%d check(s) failed\n", fails)
		return 1
	}
	fmt.Println("all checks passed")
	return 0
}
