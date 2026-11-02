// Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.

//go:build windows

package main

// The window: Edge in app mode (no address bar or tabs), with its own profile so it opens fast and
// remembers its size. Without Edge, the default browser.
//
// The profile is a different one when the app runs as administrator: an Edge started as administrator
// on a profile keeps it (Edge stays running in the background), and an Edge started normally then can't
// hand the window over to it, so it opened nothing until the app was run as administrator again.

import (
	"encoding/json"
	"fmt"
	"io"
	"unsafe"

	"golang.org/x/sys/windows"

	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

func edgePath() string {
	for _, env := range []string{"ProgramFiles(x86)", "ProgramFiles", "LocalAppData"} {
		if base := os.Getenv(env); base != "" {
			p := filepath.Join(base, "Microsoft", "Edge", "Application", "msedge.exe")
			if _, err := os.Stat(p); err == nil {
				return p
			}
		}
	}
	return ""
}

// windowProfile: Edge's profile for the app's window, one for each of normal and administrator
func windowProfile() string {
	name := "window"
	if windows.GetCurrentProcessToken().IsElevated() {
		name = "window-admin"
	}
	return filepath.Join(configDir(), name)
}

func openBrowser(url string) {
	_ = exec.Command("rundll32", "url.dll,FileProtocolHandler", url).Start()
}

func openWindow(url string) {
	if edge := edgePath(); edge != "" {
		c := exec.Command(edge, "--app="+url, "--window-size=1280,760", "--user-data-dir="+windowProfile(),
			"--no-first-run", "--disable-features=Translate")
		// not started hidden: an Edge starting fresh shows its first window the way it was started,
		// so a hidden start gave a window that was there but never showed
		if err := c.Start(); err == nil {
			winLog("edge started (profile " + filepath.Base(windowProfile()) + ")")
			return
		} else {
			winLog("edge didn't start: " + err.Error())
		}
	}
	openBrowser(url)
}

// otherInstance: the address of a copy that's already running (it answers), or ""
// an older copy still running (in the tray, say) when a new version is started: it's asked to quit,
// so the new one takes over instead of just showing the old window. "" when it went.
func replaceOlder(url string) string {
	c := http.Client{Timeout: 2 * time.Second}
	// /api/hello answers at once (/api/state asks the SS1 first, which can outlast the timeout)
	r, err := c.Get(strings.Replace(url, "/?k=", "/api/hello?k=", 1))
	if err != nil {
		return url
	}
	var st struct {
		OK bool   `json:"ok"`
		V  string `json:"app_version"` // none: a copy from before 0.2.42, so older
	}
	err = json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&st)
	r.Body.Close()
	if r.StatusCode != 200 || err != nil || !st.OK || st.V == appVersion {
		return url
	}
	if r, err := c.Post(strings.Replace(url, "/?k=", "/api/quit?k=", 1), "application/json", strings.NewReader("{}")); err == nil {
		r.Body.Close()
	}
	for i := 0; i < 50; i++ {
		time.Sleep(100 * time.Millisecond)
		if otherInstance() == "" {
			return ""
		}
	}
	return url
}

func otherInstance() string {
	b, err := os.ReadFile(instancePath())
	if err != nil {
		return ""
	}
	url := strings.TrimSpace(string(b))
	c := http.Client{Timeout: 700 * time.Millisecond}
	if r, err := c.Get(strings.Replace(url, "/?k=", "/api/hello?k=", 1)); err == nil {
		r.Body.Close()
		if r.StatusCode == 200 {
			return url
		}
	}
	return ""
}

var (
	pFindWindow       = windows.NewLazySystemDLL("user32.dll").NewProc("FindWindowW")
	pShowWindow       = windows.NewLazySystemDLL("user32.dll").NewProc("ShowWindow")
	pIsIconic         = windows.NewLazySystemDLL("user32.dll").NewProc("IsIconic")
	pIsVisible        = windows.NewLazySystemDLL("user32.dll").NewProc("IsWindowVisible")
	pSetForegroundWnd = windows.NewLazySystemDLL("user32.dll").NewProc("SetForegroundWindow")
)

// focusWindow: the app's window (the Edge app window titled appName) to the front; false if none is open
func focusWindow() bool {
	cls, _ := windows.UTF16PtrFromString("Chrome_WidgetWin_1")
	t, _ := windows.UTF16PtrFromString(appName)
	h, _, _ := pFindWindow.Call(uintptr(unsafe.Pointer(cls)), uintptr(unsafe.Pointer(t)))
	if h == 0 {
		winLog("start: no window open")
		return false
	}
	vis, _, _ := pIsVisible.Call(h)
	ic, _, _ := pIsIconic.Call(h)
	winLog(fmt.Sprintf("start: window found (visible %v, minimised %v)", vis != 0, ic != 0))
	if vis == 0 {
		pShowWindow.Call(h, 5) // SW_SHOW: a window that was there but hidden
	}
	if ic != 0 {
		pShowWindow.Call(h, 9) // SW_RESTORE
	}
	pSetForegroundWnd.Call(h)
	return true
}

// winLog: what happened opening the window, in window.log next to the settings (kept short), so a
// window that doesn't show can be looked into
func winLog(msg string) {
	p := filepath.Join(configDir(), "window.log")
	if fi, err := os.Stat(p); err == nil && fi.Size() > 64<<10 {
		_ = os.Rename(p, p+".1")
	}
	f, err := os.OpenFile(p, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		return
	}
	el := ""
	if windows.GetCurrentProcessToken().IsElevated() {
		el = " [administrator]"
	}
	fmt.Fprintf(f, "%s %s%s %s\n", time.Now().Format("2006-01-02 15:04:05"), appVersion, el, msg)
	f.Close()
}
