// Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.

package main

// Showing the window: the open one to the front (with a screen to switch to, picked up by the page),
// or a new one.

import "sync"

var view struct {
	sync.Mutex
	url, next string
}

// showUI: bring the app's window up, on screen v ("" for wherever it is, "notifications")
func showUI(v string) {
	view.Lock()
	view.next = v
	u := view.url
	view.Unlock()
	if focusWindow() {
		return
	}
	if v != "" {
		u += "#" + v
	}
	openWindow(u)
}

// takeView: the screen the page should switch to, once
func takeView() string {
	view.Lock()
	defer view.Unlock()
	v := view.next
	view.next = ""
	return v
}
