// Console Mode for Windows (cm_win): the PC app of Console Mode on the SuperStation One: the game library, box art and details, from Windows.
// Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
//
// One exe, no install. It serves its screens on 127.0.0.1 (a random port, with a key in the URL so nothing
// else on the PC can use it) and shows them in an Edge app window, laid out like Console Mode on the TV. It
// talks to the SS1 through the app link (ss1remote, ../remote), paired with the code Console Mode shows.
//
// Built from Phosphor for Windows (phosphor_os/apps/windows, also f3bandit's): the same window, tray,
// pairing and reconnecting; each feature is its own file, brought over one at a time.
package main

import (
	"crypto/rand"
	"embed"
	"encoding/hex"
	"flag"
	"fmt"
	"io/fs"
	"log"
	"net"
	"net/http"
	"os"
)

const (
	appName    = "Console Mode"             // the window's title, the tray, the Backups folder in Documents
	appFull    = "Console Mode for Windows" // how the SS1 lists this PC ("<PC name> (Console Mode for Windows)")
	configName = "Console Mode"             // %APPDATA%\<this>
	appVersion = "0.1.2"
)

//go:embed web
var webFS embed.FS

var sessionKey string

func main() {
	noWindow := flag.Bool("no-window", false, "don't open a window (print the address)")
	port := flag.Int("port", 0, "port on 127.0.0.1 (0: any free one)")
	flag.Parse()

	url := otherInstance()
	if url != "" {
		url = replaceOlder(url) // an older version: it quits, this one takes over
	}
	if url != "" && !*noWindow { // already running: show that window
		if !focusWindow() {
			openWindow(url)
		}
		return
	}
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		log.Fatal(err)
	}
	sessionKey = hex.EncodeToString(b)
	loadConfig()

	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", *port))
	if err != nil {
		log.Fatal(err)
	}
	url = fmt.Sprintf("http://%s/?k=%s", ln.Addr(), sessionKey)
	mux := http.NewServeMux()
	web, _ := fs.Sub(webFS, "web")
	mux.Handle("/", http.FileServer(http.FS(web)))
	routes(mux)
	saveInstance(url)
	view.url = url
	if *noWindow {
		fmt.Println(url)
	} else {
		openWindow(url)
	}
	srv := &http.Server{Handler: mux}
	go func() {
		if err := srv.Serve(ln); err != nil && err != http.ErrServerClosed {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	}()
	runTray(url) // the tray icon (Windows): Open, Exit. Returns when it's time to quit.
	os.Remove(instancePath())
}
