// Freeing the debug port from an old SS1 Link that never let go of it (Windows).
// Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
package main

import (
	"encoding/csv"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
)

func hidden(c *exec.Cmd) *exec.Cmd {
	c.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000} // CREATE_NO_WINDOW
	return c
}

// freeStale stops other copies of this program (an earlier session the Claude app left running
// holds the COM port open, and Windows then says "Access is denied"). Returns how many it stopped.
func freeStale() int {
	exe, err := os.Executable()
	if err != nil {
		return 0
	}
	name := filepath.Base(exe)
	out, err := hidden(exec.Command("tasklist", "/FI", "IMAGENAME eq "+name, "/FO", "CSV", "/NH")).Output()
	if err != nil {
		return 0
	}
	rows, _ := csv.NewReader(strings.NewReader(string(out))).ReadAll()
	n := 0
	for _, r := range rows {
		if len(r) < 2 || !strings.EqualFold(r[0], name) {
			continue
		}
		pid, err := strconv.Atoi(r[1])
		if err != nil || pid == os.Getpid() || pid == os.Getppid() {
			continue
		}
		if hidden(exec.Command("taskkill", "/F", "/PID", strconv.Itoa(pid))).Run() == nil {
			log.Printf("watchdog: stopped an old %s (pid %d) that was holding the debug port", name, pid)
			n++
		}
	}
	if n > 0 {
		time.Sleep(time.Second) // Windows frees the port a moment later
	}
	return n
}
