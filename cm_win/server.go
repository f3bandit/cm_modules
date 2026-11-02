// Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.

package main

// The app's own requests (from its window only: the session key, and 127.0.0.1 as the host so a web
// page elsewhere can't reach them through DNS tricks). Each feature adds its routes in its own file
// (features below); the base has the connection, the library commands and box art.

import (
	"crypto/subtle"
	"encoding/json"
	"io"
	"net/http"
	"strconv"
	"strings"
	"time"
)

func guard(f http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		k := r.Header.Get("X-App-Key")
		if k == "" {
			k = r.URL.Query().Get("k")
		}
		if !strings.HasPrefix(r.Host, "127.0.0.1:") || subtle.ConstantTimeCompare([]byte(k), []byte(sessionKey)) != 1 {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		f(w, r)
	}
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	_ = json.NewEncoder(w).Encode(v)
}

func fail(w http.ResponseWriter, msg string) { writeJSON(w, map[string]any{"ok": false, "error": msg}) }

func readJSON(r *http.Request, v any) error {
	return json.NewDecoder(io.LimitReader(r.Body, 1<<20)).Decode(v)
}

// features: each feature file adds its routes here from an init() (features = append(features, ...))
var features []func(h func(string, http.HandlerFunc))

func routes(mux *http.ServeMux) {
	h := func(p string, f http.HandlerFunc) { mux.HandleFunc(p, guard(f)) }
	h("/api/hello", func(w http.ResponseWriter, r *http.Request) { // no SS1 calls: a starting copy asks this
		writeJSON(w, map[string]any{"ok": true, "app_version": appVersion})
	})
	h("/api/state", apiState)
	h("/api/discover", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, map[string]any{"ok": true, "found": discover()})
	})
	h("/api/pair", apiPair)
	h("/api/forget", func(w http.ResponseWriter, r *http.Request) {
		_ = setPairing("", "", "", "")
		writeJSON(w, map[string]any{"ok": true})
	})
	h("/api/ping", func(w http.ResponseWriter, r *http.Request) { // the page, every few seconds while it's shown
		writeJSON(w, map[string]any{"ok": true, "view": takeView()})
	})
	h("/api/reconnect", func(w http.ResponseWriter, r *http.Request) { // GET: the setting; POST {auto} or {now: true}
		if r.Method == http.MethodPost {
			var req struct {
				Auto *bool
				Now  bool
			}
			_ = readJSON(r, &req)
			if req.Auto != nil {
				cfgMu.Lock()
				cfg.NoReconnect = !*req.Auto
				cfgMu.Unlock()
				_ = saveConfig()
			}
			if req.Now {
				if keyWorksAt(func() string { cfgMu.Lock(); defer cfgMu.Unlock(); return cfg.Address }()) {
					writeJSON(w, map[string]any{"ok": true, "found": true, "moved": false})
					return
				}
				a, ok := relocate()
				writeJSON(w, map[string]any{"ok": true, "found": ok, "moved": ok, "address": a})
				return
			}
		}
		writeJSON(w, map[string]any{"ok": true, "auto": autoReconnect()})
	})
	h("/api/quit", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, map[string]any{"ok": true})
		go func() { time.Sleep(200 * time.Millisecond); quitApp() }()
	})
	h("/api/call", apiCall)
	h("/api/art/", apiArt)
	h("/api/font/", apiFont)
	for _, f := range features {
		f(h)
	}
}

func apiState(w http.ResponseWriter, r *http.Request) {
	cfgMu.Lock()
	c := cfg
	cfgMu.Unlock()
	st := map[string]any{"ok": true, "app_version": appVersion, "app_name": appName, "address": c.Address, "name": c.Name, "paired": c.TokenEnc != ""}
	if c.Address != "" && c.TokenEnc != "" {
		if b, code, err := call(json.RawMessage(`{"cmd":"status"}`)); err == nil && code == 200 {
			st["connected"] = true
			st["status"] = json.RawMessage(b)
		} else {
			st["connected"] = false
			if err != nil {
				st["error"] = err.Error()
				st["forgotten"] = err == errForgotten
			}
		}
	}
	writeJSON(w, st)
}

func apiPair(w http.ResponseWriter, r *http.Request) {
	var req struct{ Address, Code string }
	if readJSON(r, &req) != nil || req.Address == "" || req.Code == "" {
		fail(w, "give the SS1's address and the code on the TV")
		return
	}
	addr := withPort(req.Address)
	in, err := getInfo(addr, 4*time.Second)
	if err != nil {
		fail(w, "no Console Mode app link at "+addr+": check the address on the TV (Settings > Remote access)")
		return
	}
	id, tok, err := pair(addr, req.Code)
	if err != nil {
		fail(w, err.Error())
		return
	}
	if err := setPairing(addr, in.Name, id, tok); err != nil {
		fail(w, "paired, but couldn't save it: "+err.Error())
		return
	}
	writeJSON(w, map[string]any{"ok": true})
}

// apiCall: one command, passed through (the SS1 decides what a paired device may do)
func apiCall(w http.ResponseWriter, r *http.Request) {
	body, err := io.ReadAll(io.LimitReader(r.Body, 1<<20))
	if err != nil || !json.Valid(body) {
		fail(w, "bad request")
		return
	}
	b, code, err := call(body)
	if err != nil {
		writeJSON(w, map[string]any{"ok": false, "error": err.Error(), "offline": true, "forgotten": err == errForgotten})
		return
	}
	if code != 200 && !json.Valid(b) {
		fail(w, "the SS1 answered "+strconv.Itoa(code))
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	_, _ = w.Write(b)
}

// proxyGet: a picture or file from the SS1, passed through
func proxyGet(w http.ResponseWriter, r *http.Request, path, fallbackType string, maxAge int) {
	resp, err := authed("GET", path, nil, 20*time.Second)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadGateway)
		return
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		http.NotFound(w, r)
		return
	}
	ct := resp.Header.Get("Content-Type")
	if ct == "" || ct == "application/json" {
		ct = fallbackType
	}
	w.Header().Set("Content-Type", ct)
	w.Header().Set("Cache-Control", "private, max-age="+strconv.Itoa(maxAge))
	_, _ = io.Copy(w, io.LimitReader(resp.Body, 16<<20))
}

// apiArt: a game's box art, from the SS1 (GET) or to it (PUT)
func apiArt(w http.ResponseWriter, r *http.Request) {
	which := strings.TrimPrefix(r.URL.Path, "/api/art/")
	id, err := strconv.Atoi(which)
	if err != nil || id <= 0 {
		http.NotFound(w, r)
		return
	}
	switch r.Method {
	case "GET":
		proxyGet(w, r, "/art/"+which, "image/png", 300)
	case "PUT":
		body, err := io.ReadAll(io.LimitReader(r.Body, 16<<20))
		if err != nil || len(body) == 0 {
			fail(w, "no image")
			return
		}
		resp, err := authed("PUT", "/art/"+strconv.Itoa(id), strings.NewReader(string(body)), 60*time.Second)
		if err != nil {
			fail(w, err.Error())
			return
		}
		defer resp.Body.Close()
		w.Header().Set("Content-Type", "application/json")
		_, _ = io.Copy(w, io.LimitReader(resp.Body, 1<<20))
	default:
		http.Error(w, "method", http.StatusMethodNotAllowed)
	}
}

// apiFont: Console Mode's own font, from the SS1 (so the window looks like the TV without the app
// carrying the font itself)
func apiFont(w http.ResponseWriter, r *http.Request) {
	name := strings.TrimPrefix(r.URL.Path, "/api/font/")
	if name != "regular" && name != "bold" {
		http.NotFound(w, r)
		return
	}
	proxyGet(w, r, "/font/"+name, "font/ttf", 86400)
}
