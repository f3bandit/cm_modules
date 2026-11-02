//go:build !windows

// Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
package main

// freeStale: only Windows locks a serial port to one program.
func freeStale() int { return 0 }
