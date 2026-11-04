//go:build !windows

// Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md).
package main

// freeStale: only Windows locks a serial port to one program.
func freeStale() int { return 0 }
