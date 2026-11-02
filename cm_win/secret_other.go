// Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.

//go:build !windows

package main

// Away from Windows (development and tests) the key is kept as is, in a file only this user can read.

func protect(s string) (string, error) { return "plain:" + s, nil }
func unprotect(s string) (string, error) {
	if len(s) > 6 {
		return s[6:], nil
	}
	return "", nil
}
