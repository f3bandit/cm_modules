// CM Link: talk to a MiSTer (SuperStation One debug port, or SSH) from a PC.
// Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md).
package main

import (
	"crypto/md5"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"time"
)

// Link is one way of reaching the MiSTer: its serial console, or SSH.
type Link interface {
	Name() string
	// Run a shell command (bash). Returns combined output and exit code.
	Run(cmd string, timeout time.Duration) (string, int, error)
	Push(data []byte, remote string, mode string) error
	Pull(remote string) ([]byte, error)
	Close() error
}

// Reboot is optional: only the serial link can watch the boot log.
type Rebooter interface {
	Reboot(timeout time.Duration) (string, error)
}

// Listener is optional: passive capture of whatever the console prints.
type Listener interface {
	Listen(d time.Duration, until string) (string, error)
	Recent(max int) string
}

func newID() string {
	b := make([]byte, 4)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}

func md5hex(b []byte) string {
	s := md5.Sum(b)
	return hex.EncodeToString(s[:])
}

func clip(s string, max int) string {
	if len(s) <= max {
		return s
	}
	return s[:max] + fmt.Sprintf("\n... (%d more bytes cut)", len(s)-max)
}
