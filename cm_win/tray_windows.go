// Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.

//go:build windows

package main

// The tray icon: closing the window leaves the app running (box art on the PC, watching the SS1 for
// warnings); the tray opens it again or exits completely, window and all. Warnings from the SS1 show
// as a Windows notification from the tray icon, and the icon gets a dot until they're seen.

import (
	_ "embed"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

//go:embed assets/app.ico
var trayIcon []byte

//go:embed assets/app-alert.ico
var trayIconAlert []byte

var (
	user32  = windows.NewLazySystemDLL("user32.dll")
	shell32 = windows.NewLazySystemDLL("shell32.dll")
	kern32  = windows.NewLazySystemDLL("kernel32.dll")

	pRegisterClassEx       = user32.NewProc("RegisterClassExW")
	pCreateWindowEx        = user32.NewProc("CreateWindowExW")
	pDefWindowProc         = user32.NewProc("DefWindowProcW")
	pGetMessage            = user32.NewProc("GetMessageW")
	pTranslateMessage      = user32.NewProc("TranslateMessage")
	pDispatchMessage       = user32.NewProc("DispatchMessageW")
	pPostMessage           = user32.NewProc("PostMessageW")
	pPostQuitMessage       = user32.NewProc("PostQuitMessage")
	pLoadImage             = user32.NewProc("LoadImageW")
	pCreatePopupMenu       = user32.NewProc("CreatePopupMenu")
	pAppendMenu            = user32.NewProc("AppendMenuW")
	pTrackPopupMenu        = user32.NewProc("TrackPopupMenu")
	pDestroyMenu           = user32.NewProc("DestroyMenu")
	pSetForegroundWindow   = user32.NewProc("SetForegroundWindow")
	pGetCursorPos          = user32.NewProc("GetCursorPos")
	pRegisterWindowMessage = user32.NewProc("RegisterWindowMessageW")
	pGetSystemMetrics      = user32.NewProc("GetSystemMetrics")
	pShellNotifyIcon       = shell32.NewProc("Shell_NotifyIconW")
	pSetTimer              = user32.NewProc("SetTimer")
	pKillTimer             = user32.NewProc("KillTimer")
	pGetModuleHandle       = kern32.NewProc("GetModuleHandleW")
)

const (
	wmApp           = 0x8000
	wmTray          = wmApp + 1 // the icon's own messages
	wmWake          = wmApp + 2 // something for the tray from another goroutine
	wmDestroy       = 0x0002
	wmTimer         = 0x0113
	wmCommand       = 0x0111
	wmContextMenu   = 0x007B
	ninSelect       = 0x0400
	ninKeySelect    = 0x0401
	ninBalloonClick = 0x0405
	nimAdd          = 0
	nimModify       = 1
	nimDelete       = 2
	nimSetVersion   = 4
	nifMessage      = 0x1
	nifIcon         = 0x2
	nifTip          = 0x4
	nifInfo         = 0x10
	nifShowTip      = 0x80
	niifInfo        = 0x1
	niifWarning     = 0x2
	niifUser        = 0x4
	niifLargeIcon   = 0x20
	imageIcon       = 1
	lrLoadFromFile  = 0x10
	mfString        = 0x0
	mfSeparator     = 0x800
	tpmReturnCmd    = 0x100
	tpmRightButton  = 0x2
	tpmBottomAlign  = 0x20
	smCxSmIcon      = 49
	idOpen          = 1
	idNotes         = 2
	idFiles         = 4
	idExit          = 3
	notifyIconVer4  = 4
	trayUID         = 1
	wsOverlapped    = 0
)

type notifyIconData struct {
	CbSize           uint32
	HWnd             uintptr
	UID              uint32
	UFlags           uint32
	UCallbackMessage uint32
	HIcon            uintptr
	SzTip            [128]uint16
	DwState          uint32
	DwStateMask      uint32
	SzInfo           [256]uint16
	UVersion         uint32
	SzInfoTitle      [64]uint16
	DwInfoFlags      uint32
	GuidItem         windows.GUID
	HBalloonIcon     uintptr
}

type wndClassEx struct {
	CbSize        uint32
	Style         uint32
	LpfnWndProc   uintptr
	CbClsExtra    int32
	CbWndExtra    int32
	HInstance     uintptr
	HIcon         uintptr
	HCursor       uintptr
	HbrBackground uintptr
	LpszMenuName  *uint16
	LpszClassName *uint16
	HIconSm       uintptr
}

type msg struct {
	HWnd    uintptr
	Message uint32
	WParam  uintptr
	LParam  uintptr
	Time    uint32
	Pt      struct{ X, Y int32 }
}

var lastClick time.Time

type trayNote struct {
	title, body string
	warn        bool
}

var tray struct {
	sync.Mutex
	hwnd                           uintptr
	icon, alert, large, alertLarge uintptr
	alerting                       bool
	url                            string
	notes                          []trayNote // waiting to be shown (from other goroutines)
	setAlert                       *bool
	taskbarCreated                 uint32
	retries                        int
	quitting                       bool
}

func utf16Into(dst []uint16, s string) {
	u, _ := windows.UTF16FromString(s)
	if len(u) > len(dst) {
		u = append(u[:len(dst)-1], 0)
	}
	copy(dst, u)
}

func loadIcon(data []byte, name string, size int) uintptr {
	p := filepath.Join(configDir(), name)
	if b, err := os.ReadFile(p); err != nil || string(b) != string(data) {
		_ = os.MkdirAll(configDir(), 0o700)
		_ = os.WriteFile(p, data, 0o600)
	}
	pp, _ := windows.UTF16PtrFromString(p)
	h, _, _ := pLoadImage.Call(0, uintptr(unsafe.Pointer(pp)), imageIcon, uintptr(size), uintptr(size), lrLoadFromFile)
	if h == 0 { // couldn't write or read the file: the exe's own icon (winres.json's "APP")
		inst, _, _ := pGetModuleHandle.Call(0)
		app, _ := windows.UTF16PtrFromString("APP")
		h, _, _ = pLoadImage.Call(inst, uintptr(unsafe.Pointer(app)), imageIcon, uintptr(size), uintptr(size), 0)
	}
	return h
}

func nid() notifyIconData {
	var n notifyIconData
	n.CbSize = uint32(unsafe.Sizeof(n))
	n.HWnd = tray.hwnd
	n.UID = trayUID
	return n
}

func trayAdd() {
	n := nid()
	n.UFlags = nifMessage | nifIcon | nifTip | nifShowTip
	n.UCallbackMessage = wmTray
	n.HIcon = tray.icon
	if tray.alerting {
		n.HIcon = tray.alert
	}
	utf16Into(n.SzTip[:], tipText())
	if r, _, _ := pShellNotifyIcon.Call(nimAdd, uintptr(unsafe.Pointer(&n))); r == 0 && tray.retries < 10 {
		tray.retries++ // the taskbar isn't ready yet (the app started with Windows): again in a second
		pSetTimer.Call(tray.hwnd, 1, 1000, 0)
		return
	}
	n.UVersion = notifyIconVer4
	pShellNotifyIcon.Call(nimSetVersion, uintptr(unsafe.Pointer(&n)))
}

func tipText() string {
	if tray.alerting {
		return appName + ": something needs a look"
	}
	return appName
}

func trayIconSet(alert bool) {
	tray.alerting = alert
	n := nid()
	n.UFlags = nifIcon | nifTip | nifShowTip
	n.HIcon = tray.icon
	if alert {
		n.HIcon = tray.alert
	}
	utf16Into(n.SzTip[:], tipText())
	pShellNotifyIcon.Call(nimModify, uintptr(unsafe.Pointer(&n)))
}

func trayBalloon(t trayNote) {
	n := nid()
	n.UFlags = nifInfo | nifTip | nifShowTip
	utf16Into(n.SzTip[:], tipText())
	utf16Into(n.SzInfoTitle[:], t.title)
	utf16Into(n.SzInfo[:], t.body)
	n.DwInfoFlags = niifUser | niifLargeIcon
	n.HBalloonIcon = tray.large
	if t.warn {
		n.HBalloonIcon = tray.alertLarge
	}
	if n.HBalloonIcon == 0 {
		n.DwInfoFlags = niifInfo
		if t.warn {
			n.DwInfoFlags = niifWarning
		}
	}
	pShellNotifyIcon.Call(nimModify, uintptr(unsafe.Pointer(&n)))
}

func trayMenu(x, y int32) {
	m, _, _ := pCreatePopupMenu.Call()
	add := func(id int, s string) {
		p, _ := windows.UTF16PtrFromString(s)
		pAppendMenu.Call(m, mfString, uintptr(id), uintptr(unsafe.Pointer(p)))
	}
	add(idOpen, "Open "+appName)
	pAppendMenu.Call(m, mfSeparator, 0, 0)
	add(idExit, "Exit "+appName)
	pSetForegroundWindow.Call(tray.hwnd)
	cmd, _, _ := pTrackPopupMenu.Call(m, tpmReturnCmd|tpmRightButton|tpmBottomAlign, uintptr(x), uintptr(y), 0, tray.hwnd, 0)
	pPostMessage.Call(tray.hwnd, 0, 0, 0) // WM_NULL: the menu closes properly next time (KB135788)
	pDestroyMenu.Call(m)
	switch cmd {
	case idOpen:
		showUI("")
	case idExit:
		quitApp()
	}
}

func wndProc(hwnd uintptr, m uint32, wp, lp uintptr) uintptr {
	switch {
	case m == wmTray:
		switch uint32(lp & 0xffff) {
		case ninSelect, ninKeySelect: // a click (version 4 icons send this as well as the mouse messages)
			if time.Since(lastClick) > 1500*time.Millisecond { // a double-click is one
				lastClick = time.Now()
				showUI("")
			}
		case ninBalloonClick:
			showUI("")
		case wmContextMenu: // also from the keyboard; wParam is where to put the menu
			trayMenu(int32(int16(wp&0xffff)), int32(int16((wp>>16)&0xffff)))
		}
		return 0
	case m == wmWake:
		tray.Lock()
		notes := tray.notes
		tray.notes = nil
		set := tray.setAlert
		tray.setAlert = nil
		tray.Unlock()
		if set != nil {
			trayIconSet(*set)
		}
		for _, t := range notes {
			trayBalloon(t)
		}
		return 0
	case m == tray.taskbarCreated && m != 0: // Explorer restarted: the icon again
		trayAdd()
		return 0
	case m == wmTimer:
		pKillTimer.Call(hwnd, 1)
		trayAdd()
		return 0
	case m == wmDestroy:
		n := nid()
		pShellNotifyIcon.Call(nimDelete, uintptr(unsafe.Pointer(&n)))
		pPostQuitMessage.Call(0)
		return 0
	}
	r, _, _ := pDefWindowProc.Call(hwnd, uintptr(m), wp, lp)
	return r
}

// trayNotify: a Windows notification from the tray icon (safe from any goroutine)
func trayNotify(title, body string, warn bool) {
	tray.Lock()
	tray.notes = append(tray.notes, trayNote{title, body, warn})
	h := tray.hwnd
	tray.Unlock()
	if h != 0 {
		pPostMessage.Call(h, wmWake, 0, 0)
	}
}

// trayAlert: the dot on the tray icon while there are warnings nobody has looked at
func trayAlert(on bool) {
	tray.Lock()
	tray.setAlert = &on
	h := tray.hwnd
	tray.Unlock()
	if h != 0 {
		pPostMessage.Call(h, wmWake, 0, 0)
	}
}

func quitApp() {
	tray.Lock()
	if tray.quitting {
		tray.Unlock()
		return
	}
	tray.quitting = true
	tray.Unlock()
	go func() { closeWindows(); quitTray() }()
}

func quitTray() {
	tray.Lock()
	h := tray.hwnd
	tray.Unlock()
	if h != 0 {
		pPostMessage.Call(h, 0x0010, 0, 0) // WM_CLOSE -> DefWindowProc destroys -> WM_DESTROY
	} else {
		os.Exit(0)
	}
}

// runTray: the icon and its message loop, on this (locked) thread, until Exit
func runTray(url string) {
	runtime.LockOSThread()
	tray.url = url
	small, _, _ := pGetSystemMetrics.Call(smCxSmIcon)
	if small == 0 {
		small = 16
	}
	tray.icon = loadIcon(trayIcon, "tray.ico", int(small))
	tray.alert = loadIcon(trayIconAlert, "tray-alert.ico", int(small))
	tray.large = loadIcon(trayIcon, "tray.ico", 48)
	tray.alertLarge = loadIcon(trayIconAlert, "tray-alert.ico", 48)
	inst, _, _ := pGetModuleHandle.Call(0)
	cls, _ := windows.UTF16PtrFromString("ConsoleModeWinTray")
	wc := wndClassEx{LpfnWndProc: windows.NewCallback(wndProc), HInstance: inst, LpszClassName: cls}
	wc.CbSize = uint32(unsafe.Sizeof(wc))
	pRegisterClassEx.Call(uintptr(unsafe.Pointer(&wc)))
	tb, _ := windows.UTF16PtrFromString("TaskbarCreated")
	r, _, _ := pRegisterWindowMessage.Call(uintptr(unsafe.Pointer(tb)))
	tray.taskbarCreated = uint32(r)
	title, _ := windows.UTF16PtrFromString(appName + " tray")
	h, _, _ := pCreateWindowEx.Call(0, uintptr(unsafe.Pointer(cls)), uintptr(unsafe.Pointer(title)), wsOverlapped, 0, 0, 0, 0, 0, 0, inst, 0)
	if h == 0 {
		select {} // no tray (shouldn't happen): keep serving; Exit in Settings still works
	}
	tray.Lock()
	tray.hwnd = h
	pending := len(tray.notes) > 0 || tray.setAlert != nil
	tray.Unlock()
	trayAdd()
	if pending {
		pPostMessage.Call(h, wmWake, 0, 0)
	}
	var m msg
	for {
		ret, _, _ := pGetMessage.Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0)
		if int32(ret) <= 0 {
			break
		}
		pTranslateMessage.Call(uintptr(unsafe.Pointer(&m)))
		pDispatchMessage.Call(uintptr(unsafe.Pointer(&m)))
	}
}

// closeWindows: the Edge windows on the app's own window profile (never the user's own Edge)
func closeWindows() {
	// this copy's profile only (the administrator one is "window-admin", which "window" is the start of)
	prof := strings.ToLower(windowProfile())
	ps := `$p = [regex]::Escape('` + strings.ReplaceAll(prof, "'", "''") + `') + '(["\s]|$)'; ` +
		`Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" | Where-Object { $_.CommandLine -and $_.CommandLine.ToLower() -match $p } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }`
	c := exec.Command("powershell", "-NoProfile", "-NonInteractive", "-Command", ps)
	c.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000}
	_ = c.Run()
}

func openFolder(p string) { _ = exec.Command("explorer.exe", p).Start() }
