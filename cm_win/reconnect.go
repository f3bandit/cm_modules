// Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.

package main

// Reconnecting by itself: when the SS1 stops answering at its address (a restart, or the router gave it
// a new address), the app looks on the home network for it and moves over, without pairing again.
// A host found on the network never gets the key until it proves it already knows it: /info?nonce=
// answers with proofs (sha256(sha256(key) + ":" + nonce), one per paired device), and only an SS1 whose
// proofs hold this PC's is adopted. Older SS1s without proofs aren't moved to automatically.
// On by default (Settings > Connection).

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/url"
	"sync"
	"time"
)

var relocMu sync.Mutex

// keyWorksAt: does the SS1 at addr know this PC's key? (one cheap request, nothing changed)
func keyWorksAt(addr string) bool {
	tok := token()
	if tok == "" {
		return false
	}
	req, err := http.NewRequest("POST", "http://"+withPort(addr)+"/api", bytes.NewReader([]byte(`{"cmd":"gen"}`)))
	if err != nil {
		return false
	}
	req.Header.Set("Authorization", "Bearer "+tok)
	r, err := client(3 * time.Second).Do(req)
	if err != nil {
		return false
	}
	_, _ = io.Copy(io.Discard, io.LimitReader(r.Body, 4096))
	r.Body.Close()
	return r.StatusCode == 200
}

// keyProof: what an SS1 that knows tok answers for nonce
func keyProof(tok, nonce string) string {
	h := sha256.Sum256([]byte(tok))
	p := sha256.Sum256([]byte(hex.EncodeToString(h[:]) + ":" + nonce))
	return hex.EncodeToString(p[:])
}

// provesKey: does the the app at addr show it knows this PC's key, without the key being sent?
func provesKey(addr string) bool {
	tok := token()
	if tok == "" {
		return false
	}
	nb := make([]byte, 16)
	if _, err := rand.Read(nb); err != nil {
		return false
	}
	nonce := hex.EncodeToString(nb)
	r, err := client(3 * time.Second).Get("http://" + withPort(addr) + "/info?nonce=" + url.QueryEscape(nonce))
	if err != nil {
		return false
	}
	defer r.Body.Close()
	var in struct {
		info
		Proofs []string `json:"proofs"`
	}
	if r.StatusCode != 200 || json.NewDecoder(io.LimitReader(r.Body, 64<<10)).Decode(&in) != nil || !in.known() {
		return false
	}
	want := keyProof(tok, nonce)
	for _, p := range in.Proofs { // none (an older SS1): not adopted
		if p == want {
			return true
		}
	}
	return false
}

func autoReconnect() bool {
	cfgMu.Lock()
	defer cfgMu.Unlock()
	return !cfg.NoReconnect
}

// relocate: find this PC's SS1 on the network; true (and the new address saved) when it moved
func relocate() (string, bool) {
	relocMu.Lock()
	defer relocMu.Unlock()
	cfgMu.Lock()
	cur := cfg.Address
	cfgMu.Unlock()
	if cur == "" || token() == "" {
		return "", false
	}
	for _, f := range discover() {
		a := withPort(f.Address)
		if a == withPort(cur) {
			continue
		}
		if provesKey(a) && keyWorksAt(a) { // the key goes only to a host that proved it has it
			cfgMu.Lock()
			cfg.Address = a
			cfgMu.Unlock()
			_ = saveConfig()
			return a, true
		}
	}
	return "", false
}
