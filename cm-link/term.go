// A plain terminal on the serial console, for a person (Ctrl-] quits).
// Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md).
package main

import (
	"fmt"
	"os"

	"golang.org/x/term"
)

func runTerm(cfg Config) error {
	name, err := findPort(cfg)
	if err != nil {
		return err
	}
	p, err := openRetry(name, cfg.Baud)
	if err != nil {
		return err
	}
	defer p.Close()
	fmt.Fprintf(os.Stderr, "Connected to %s at %d. Ctrl-] quits. Don't press keys while the MiSTer is booting.\r\n", name, cfg.Baud)
	fd := int(os.Stdin.Fd())
	if term.IsTerminal(fd) {
		old, err := term.MakeRaw(fd)
		if err == nil {
			defer term.Restore(fd, old)
		}
	}
	done := make(chan error, 2)
	go func() {
		b := make([]byte, 4096)
		for {
			n, err := p.Read(b)
			if n > 0 {
				_, _ = os.Stdout.Write(b[:n])
			}
			if err != nil {
				done <- err
				return
			}
		}
	}()
	go func() {
		b := make([]byte, 256)
		for {
			n, err := os.Stdin.Read(b)
			if err != nil {
				done <- err
				return
			}
			for i := 0; i < n; i++ {
				if b[i] == 0x1d {
					done <- nil
					return
				}
			}
			if _, err := p.Write(b[:n]); err != nil {
				done <- err
				return
			}
		}
	}()
	err = <-done
	fmt.Fprint(os.Stderr, "\r\nDisconnected.\r\n")
	return err
}
