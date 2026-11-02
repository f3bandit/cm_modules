// Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.

package main

// Backups: one folder for everything this app keeps. On the PC that's Documents\Console Mode\Backups
// (or a folder the user picks), one subfolder per kind; on the SS1 it's /media/fat/ConsoleMode/ss1/backups
// ({"cmd":"backups"} from the app link). A tool that backs something up asks backupDir(kind) for its folder
// and never writes anywhere else.

import (
	"encoding/json"
	"io/fs"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

// the kinds, the same names on the PC and on the SS1 where both have them
var backupKinds = []struct{ ID, Name string }{
	{"mister-linux", "MiSTer & Linux backups"},
	{"memory-cards", "Memory cards"},
	{"saves", "Saves"},
	{"controllers", "Controllers"},
	{"sd-cards", "SD card images"},
}

func init() { features = append(features, func(h func(string, http.HandlerFunc)) { h("/api/backups", apiBackups) }) }

// backupsRoot: the PC's backups folder
func backupsRoot() string {
	cfgMu.Lock()
	d := cfg.BackupsDir
	cfgMu.Unlock()
	if d != "" {
		return d
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return filepath.Join(configDir(), "Backups")
	}
	return filepath.Join(home, "Documents", appName, "Backups")
}

// backupDir: a kind's folder on the PC, made if need be
func backupDir(kind string) (string, error) {
	d := filepath.Join(backupsRoot(), kind)
	return d, os.MkdirAll(d, 0o755)
}

func dirSize(p string) int64 {
	var n int64
	_ = filepath.WalkDir(p, func(_ string, d fs.DirEntry, err error) error {
		if err == nil && !d.IsDir() {
			if i, e := d.Info(); e == nil {
				n += i.Size()
			}
		}
		return nil
	})
	return n
}

func openFolderOf(p string) {
	_ = os.MkdirAll(p, 0o755)
	if runtime.GOOS == "windows" {
		_ = exec.Command("explorer.exe", p).Start()
	}
}

// GET: both folders and what's in them. POST {open:"pc"|kind} opens it in Explorer; {folder:"..."} moves
// where new backups go ("" back to the default)
func apiBackups(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodPost {
		var req struct {
			Open   string  `json:"open"`
			Folder *string `json:"folder"`
		}
		if readJSON(r, &req) != nil {
			fail(w, "bad request")
			return
		}
		if req.Folder != nil {
			if *req.Folder != "" && !filepath.IsAbs(*req.Folder) {
				fail(w, "give a full folder path")
				return
			}
			cfgMu.Lock()
			cfg.BackupsDir = *req.Folder
			cfgMu.Unlock()
			_ = saveConfig()
		}
		if req.Open != "" {
			p := backupsRoot()
			for _, k := range backupKinds {
				if k.ID == req.Open {
					p = filepath.Join(p, k.ID)
				}
			}
			openFolderOf(p)
		}
	}
	root := backupsRoot()
	_ = os.MkdirAll(root, 0o755)
	kinds := []map[string]any{}
	var all int64
	for _, k := range backupKinds {
		p := filepath.Join(root, k.ID)
		n := dirSize(p)
		all += n
		_, err := os.Stat(p)
		kinds = append(kinds, map[string]any{"id": k.ID, "name": k.Name, "path": p, "exists": err == nil, "bytes": n})
	}
	out := map[string]any{"ok": true, "pc": map[string]any{"folder": root, "kinds": kinds, "bytes": all}}
	if b, code, err := call(json.RawMessage(`{"cmd":"backups"}`)); err == nil && code == 200 {
		out["ss1"] = json.RawMessage(b)
	}
	writeJSON(w, out)
}
