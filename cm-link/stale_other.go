//go:build !windows

// Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see ../LICENSE.md).
package main

// freeStale: only Windows locks a serial port to one program.
func freeStale() int { return 0 }
