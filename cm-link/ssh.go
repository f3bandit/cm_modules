// CM Link over the network (SSH), when the serial port isn't there.
// Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"errors"
	"fmt"
	"log"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"golang.org/x/crypto/ssh"
)

type SSHLink struct {
	host string
	c    *ssh.Client
	dead atomic.Bool // the keepalive watchdog lost the MiSTer
	stop chan struct{}
}

func OpenSSH(cfg Config) (*SSHLink, error) {
	if cfg.Host == "" {
		return nil, errors.New("no host set (host= in cm-link.ini)")
	}
	addr := cfg.Host
	if !strings.Contains(addr, ":") {
		addr += ":22"
	}
	conf := &ssh.ClientConfig{
		User: cfg.SSHUser,
		Auth: append([]ssh.AuthMethod{}, ssh.Password(cfg.SSHPassword), ssh.KeyboardInteractive(
			func(user, instr string, qs []string, echos []bool) ([]string, error) {
				a := make([]string, len(qs))
				for i := range a {
					a[i] = cfg.SSHPassword
				}
				return a, nil
			})),
		// Every stock MiSTer ships the same host keys and a reflash makes new ones, so pinning them
		// would only break the link after each reflash. A changed key is accepted and logged.
		HostKeyCallback: noteHostKey,
		Timeout:         5 * time.Second,
	}
	// the time limit covers the handshake too: a MiSTer half-way through booting or shutting down can
	// take the connection and then never answer, which would hang every call behind this one
	conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		return nil, fmt.Errorf("ssh %s: %w", addr, err)
	}
	_ = conn.SetDeadline(time.Now().Add(15 * time.Second))
	cc, chans, reqs, err := ssh.NewClientConn(conn, addr, conf)
	if err != nil {
		conn.Close()
		return nil, fmt.Errorf("ssh %s: %w", addr, err)
	}
	_ = conn.SetDeadline(time.Time{})
	c := ssh.NewClient(cc, chans, reqs)
	l := &SSHLink{host: addr, c: c, stop: make(chan struct{})}
	go l.keepalive()
	return l, nil
}

// keepalive: the watchdog. A MiSTer that rebooted or dropped off the network stops answering;
// mark the link dead (and close it, so a stuck call returns) and the next call reconnects.
func (l *SSHLink) keepalive() {
	t := time.NewTicker(10 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-l.stop:
			return
		case <-t.C:
		}
		res := make(chan error, 1)
		go func() { _, _, err := l.c.SendRequest("keepalive@openssh.com", true, nil); res <- err }()
		var err error
		select {
		case err = <-res:
		case <-time.After(8 * time.Second):
			err = errors.New("no answer")
		}
		if err != nil {
			log.Printf("watchdog: %s stopped answering (%v), will reconnect", l.host, err)
			l.dead.Store(true)
			_ = l.c.Close()
			return
		}
	}
}

func (l *SSHLink) Alive() bool { return !l.dead.Load() }

// noteHostKey keeps the last key seen per host next to the program and logs when it changes
// (a reflash or another MiSTer), instead of refusing.
func noteHostKey(host string, _ net.Addr, key ssh.PublicKey) error {
	sum := sha256.Sum256(key.Marshal())
	fp := "SHA256:" + base64.RawStdEncoding.EncodeToString(sum[:])
	f := filepath.Join(exeDir(), "cm-link.hostkeys")
	b, _ := os.ReadFile(f)
	lines := strings.Split(strings.TrimSpace(string(b)), "\n")
	for i, ln := range lines {
		h, old, ok := strings.Cut(ln, " ")
		if !ok || h != host {
			continue
		}
		if old != fp {
			log.Printf("%s has a new host key (%s, was %s): a reflash, or another MiSTer at that address", host, fp, old)
			lines[i] = host + " " + fp
			_ = os.WriteFile(f, []byte(strings.TrimSpace(strings.Join(lines, "\n"))+"\n"), 0644)
		}
		return nil
	}
	lines = append(lines, host+" "+fp)
	_ = os.WriteFile(f, []byte(strings.TrimSpace(strings.Join(lines, "\n"))+"\n"), 0644)
	return nil
}

// syncBuf: stdout and stderr are copied by two goroutines into one buffer; a plain bytes.Buffer
// loses output when they write at the same time.
type syncBuf struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (w *syncBuf) Write(p []byte) (int, error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.b.Write(p)
}

func (w *syncBuf) String() string {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.b.String()
}

func (l *SSHLink) Name() string { return "ssh " + l.host }

func (l *SSHLink) session() (*ssh.Session, error) { return l.c.NewSession() }

func (l *SSHLink) Run(cmd string, timeout time.Duration) (string, int, error) {
	s, err := l.session()
	if err != nil {
		return "", -1, err
	}
	defer s.Close()
	var out syncBuf
	s.Stdout, s.Stderr = &out, &out
	s.Stdin = strings.NewReader(cmd + "\n")
	done := make(chan error, 1)
	go func() { done <- s.Run("bash -s") }()
	select {
	case err = <-done:
	case <-time.After(timeout):
		_ = s.Signal(ssh.SIGKILL)
		return out.String(), -1, fmt.Errorf("command timed out after %s", timeout)
	}
	if err == nil {
		return out.String(), 0, nil
	}
	var ee *ssh.ExitError
	if errors.As(err, &ee) {
		return out.String(), ee.ExitStatus(), nil
	}
	return out.String(), -1, err
}

func (l *SSHLink) Push(data []byte, remote, mode string) error {
	s, err := l.session()
	if err != nil {
		return err
	}
	defer s.Close()
	if mode == "" {
		mode = "644"
	}
	r := sq(remote)
	s.Stdin = bytes.NewReader(data)
	var out syncBuf
	s.Stdout, s.Stderr = &out, &out
	if err := runFor(s, "mkdir -p \"$(dirname "+r+")\" && cat > "+r+".pltmp && mv -f "+r+".pltmp "+r+" && chmod "+mode+" "+r+" && md5sum < "+r, sizeTime(len(data))); err != nil {
		return fmt.Errorf("writing %s: %v %s", remote, err, out.String())
	}
	if !strings.HasPrefix(out.String(), md5hex(data)) {
		return fmt.Errorf("%s arrived damaged", remote)
	}
	return nil
}

func (l *SSHLink) Pull(remote string) ([]byte, error) {
	s, err := l.session()
	if err != nil {
		return nil, err
	}
	defer s.Close()
	var out, errb bytes.Buffer
	s.Stdout, s.Stderr = &out, &errb
	if err := runFor(s, "cat "+sq(remote), 10*time.Minute); err != nil {
		if e := strings.TrimSpace(errb.String()); e != "" {
			return nil, fmt.Errorf("reading %s: %s", remote, e)
		}
		return nil, fmt.Errorf("reading %s: %v", remote, err)
	}
	return out.Bytes(), nil
}

// sizeTime: time allowed to send n bytes (at a slow 200 KB/s, plus a minute).
func sizeTime(n int) time.Duration { return time.Minute + time.Duration(n/200_000)*time.Second }

// runFor: Run with a time limit, so a MiSTer that vanished mid-copy can't hang the call.
func runFor(s *ssh.Session, cmd string, d time.Duration) error {
	done := make(chan error, 1)
	go func() { done <- s.Run(cmd) }()
	select {
	case err := <-done:
		return err
	case <-time.After(d):
		_ = s.Signal(ssh.SIGKILL)
		_ = s.Close()
		return fmt.Errorf("timed out after %s", d)
	}
}

func (l *SSHLink) Close() error {
	select {
	case <-l.stop:
	default:
		close(l.stop)
	}
	return l.c.Close()
}

