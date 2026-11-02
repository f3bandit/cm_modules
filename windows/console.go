// Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.

package main

// Talking to the SS1: the app link (ss1remote, remote/ in ss1_cm_modules; the same requests as the app's remote access). Plain HTTP on the home network,
// one request per connection, the pairing key as a Bearer token.

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math/rand"
	"net"
	"net/http"
	"os"
	"strings"
	"sync/atomic"
	"syscall"
	"time"
)

const remotePort = "8767"

var (
	errNotPaired = errors.New("not paired")
	errForgotten = errors.New("the SS1 doesn't know this PC any more: pair again")
)

func client(timeout time.Duration) *http.Client {
	return &http.Client{Timeout: timeout, Transport: &http.Transport{DisableKeepAlives: true, Proxy: nil}}
}

func withPort(addr string) string {
	addr = strings.TrimSpace(strings.TrimPrefix(strings.TrimPrefix(addr, "http://"), "https://"))
	addr = strings.TrimSuffix(addr, "/")
	if addr != "" && !strings.Contains(addr, ":") {
		addr += ":" + remotePort
	}
	return addr
}

type info struct {
	OK       bool   `json:"ok"`
	Name     string `json:"name"`
	Frontend string `json:"frontend"`
	Version  string `json:"version"`
	API      int    `json:"api"`
}

// known: an SS1 running Console Mode with the app link (ss1remote says "frontend":"Console Mode",
// whatever name it gives itself)
func (in info) known() bool { return in.OK && in.Frontend == "Console Mode" }

// getInfo: is there an SS1 with the app link at addr? (no key needed)
func getInfo(addr string, timeout time.Duration) (info, error) {
	var in info
	r, err := client(timeout).Get("http://" + withPort(addr) + "/info")
	if err != nil {
		return in, err
	}
	defer r.Body.Close()
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&in); err != nil || !in.OK || !in.known() {
		return in, errors.New("no app link there")
	}
	return in, nil
}

// pair: the code from the TV for a key
func pair(addr, code string) (id, tok string, err error) {
	host, _ := os.Hostname()
	if host == "" {
		host = "Windows PC"
	}
	body, _ := json.Marshal(map[string]string{"code": strings.TrimSpace(code), "name": host + " (" + appName + ")"})
	r, err := client(8*time.Second).Post("http://"+withPort(addr)+"/pair", "application/json", bytes.NewReader(body))
	if err != nil {
		return "", "", fmt.Errorf("can't reach the SS1: %v", err)
	}
	defer r.Body.Close()
	var out struct {
		OK    bool   `json:"ok"`
		Token string `json:"token"`
		ID    string `json:"id"`
		Error string `json:"error"`
	}
	_ = json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&out)
	switch {
	case r.StatusCode == 401:
		return "", "", errors.New("that code didn't work: check it on the TV")
	case r.StatusCode == 403:
		return "", "", errors.New("pairing isn't open: choose Pair a device on the TV first")
	case r.StatusCode == 429:
		return "", "", errors.New("the SS1 already has 16 paired devices: forget one in Settings > Remote access")
	case !out.OK || out.Token == "":
		if out.Error != "" {
			return "", "", errors.New(out.Error)
		}
		return "", "", fmt.Errorf("pairing failed (%d)", r.StatusCode)
	}
	return out.ID, out.Token, nil
}

// busyTries: how many more times a request the SS1 was too busy for is sent (about 30 seconds in all)
var busyTries = 20 // (a var: the end-to-end test can turn it off)

// busyRetries: how many times a request was sent again (counted for the tests)
var busyRetries atomic.Int64

// cutOffs: uploads sent again after their connection was cut off (counted for the tests)
var cutOffs atomic.Int64

// busyWait: a short wait, a little longer each time, with some jitter so waiting requests don't all come back at once
func busyWait(try int) time.Duration {
	ms := 250 * (try + 1)
	if ms > 2000 {
		ms = 2000
	}
	return time.Duration(ms+rand.Intn(250)) * time.Millisecond
}

// notSent: the connection died before the request went out (Go: "readLoopPeekFailLocked", an answer
// arrived before the request was written, as a busy SS1 used to send it): nothing reached the SS1, so
// any request can be sent again
func notSent(err error) bool {
	return strings.Contains(err.Error(), "readLoopPeekFailLocked")
}

// cutOff: the connection was made and then reset or closed before an answer (not: no connection at all,
// or no answer in time)
func cutOff(err error) bool {
	var op *net.OpError
	if errors.As(err, &op) && op.Op == "dial" {
		return false
	}
	if errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
		return false
	}
	var ne net.Error
	if errors.As(err, &ne) && ne.Timeout() {
		return false
	}
	return errors.Is(err, syscall.ECONNRESET) || errors.Is(err, syscall.EPIPE) || errors.Is(err, io.EOF) ||
		errors.Is(err, io.ErrUnexpectedEOF) || strings.Contains(err.Error(), "readLoopPeekFailLocked") ||
		strings.Contains(err.Error(), "server closed") || strings.Contains(err.Error(), "forcibly closed") || strings.Contains(err.Error(), "connection reset")
}

func authed(method, path string, body io.Reader, timeout time.Duration) (*http.Response, error) {
	cfgMu.Lock()
	addr := cfg.Address
	cfgMu.Unlock()
	tok := token()
	if addr == "" || tok == "" {
		return nil, errNotPaired
	}
	// the SS1 serves a few connections at once and answers 503 "busy" to more: kept, so it can be sent again
	var data []byte
	if body != nil {
		b, err := io.ReadAll(body)
		if err != nil {
			return nil, err
		}
		data = b
	}
	var r *http.Response
	for try := 0; ; try++ {
		var rd io.Reader
		if body != nil {
			rd = bytes.NewReader(data)
		}
		req, err := http.NewRequest(method, "http://"+addr+path, rd)
		if err != nil {
			return nil, err
		}
		req.Header.Set("Authorization", "Bearer "+tok)
		r, err = client(timeout).Do(req)
		if err != nil {
			// a busy SS1 answering a big upload (box art, a package) drops the rest of it, and the dropped
			// connection can cut off the "busy" answer: an upload is sent again (it only replaces a file)
			if try < busyTries && (notSent(err) || (method == http.MethodPut && cutOff(err))) {
				busyRetries.Add(1)
				cutOffs.Add(1)
				time.Sleep(busyWait(try))
				continue
			}
			return nil, err
		}
		// only the "busy" answer (sent before the request is read, so nothing was done) has Retry-After
		if r.StatusCode != 503 || r.Header.Get("Retry-After") == "" || try >= busyTries {
			break
		}
		io.Copy(io.Discard, io.LimitReader(r.Body, 4096))
		r.Body.Close()
		busyRetries.Add(1)
		time.Sleep(busyWait(try))
	}
	if r.StatusCode == 401 {
		r.Body.Close()
		return nil, errForgotten
	}
	return r, nil
}

// call: one command (JSON), its reply (JSON)
func call(cmd json.RawMessage) (json.RawMessage, int, error) {
	return callFor(cmd, 30*time.Second)
}

// callFor: a command that can take longer (Update All's settings: its code starts on the SS1 first)
func callFor(cmd json.RawMessage, timeout time.Duration) (json.RawMessage, int, error) {
	r, err := authed("POST", "/api", bytes.NewReader(cmd), timeout)
	if err != nil {
		return nil, 0, err
	}
	defer r.Body.Close()
	b, err := io.ReadAll(io.LimitReader(r.Body, 32<<20))
	return b, r.StatusCode, err
}
