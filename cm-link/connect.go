// CM Link: picking a way to reach the MiSTer, keeping it open, and healing it when it dies.
// Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md).
package main

import (
	"errors"
	"fmt"
	"log"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

// sshHost: host= from cm-link.ini (the SuperStation's address on the network).
func sshHost(cfg Config) string { return cfg.Host }

func connect(cfg Config) (Link, error) {
	switch strings.ToLower(cfg.Transport) {
	case "serial":
		return OpenSerial(cfg)
	case "ssh":
		cfg.Host = sshHost(cfg)
		return OpenSSH(cfg)
	}
	// auto: SSH first when the SuperStation is on the network (fast, no console noise, big files are quick),
	// the debug port otherwise (works with no network, and shows the boot)
	var herr error
	if h := sshHost(cfg); h != "" {
		c := cfg
		c.Host = h
		l, err := OpenSSH(c)
		if err == nil {
			return l, nil
		}
		herr = err
	}
	s, serr := OpenSerial(cfg)
	if serr == nil {
		return s, nil
	}
	if herr == nil {
		return nil, fmt.Errorf("serial: %v (SSH not tried: no host= in cm-link.ini)", serr)
	}
	return nil, fmt.Errorf("ssh: %v; serial: %v", herr, serr)
}

// Checker is optional: a link that can tell whether it still answers.
type Checker interface {
	Alive() bool
}

// session keeps one link open between calls, plus the debug port as the console (for the boot log,
// reboots, and to keep the dock's USB serial bridge awake), and reopens either one if it dies.
//
// There is only ever one SerialLink: the console. When SSH is down, the console is the main link
// too. Opening the debug port is done by one caller at a time (openMu), never while holding mu, so
// a login that waits for a boot doesn't hold up calls that can go over SSH.
type session struct {
	mu     sync.Mutex
	openMu sync.Mutex
	cfg    Config
	link   Link        // the main link: SSH, or the console
	serial *SerialLink // the console, or nil
	port   string      // the debug port, once found (only that one is tried again)
	sshTry time.Time   // the last time SSH was tried while the main link was the console
	keepGo sync.Once
}

func (s *session) wantSSH() bool    { return strings.ToLower(s.cfg.Transport) != "serial" }
func (s *session) wantSerial() bool { return strings.ToLower(s.cfg.Transport) != "ssh" }

func (s *session) openSSH() (Link, error) {
	h := sshHost(s.cfg)
	if h == "" {
		return nil, errors.New("no host= in cm-link.ini")
	}
	c := s.cfg
	c.Host = h
	return OpenSSH(c)
}

func (s *session) get() (Link, error) {
	s.mu.Lock()
	l := s.link
	if l != nil {
		if c, ok := l.(Checker); ok && !c.Alive() {
			log.Printf("watchdog: %s stopped answering, reconnecting", l.Name())
			s.forget(l)
			l = nil
		}
	}
	retrySSH := false
	if _, onConsole := l.(*SerialLink); onConsole && s.wantSSH() && time.Since(s.sshTry) > 20*time.Second {
		s.sshTry = time.Now()
		retrySSH = true
	}
	s.mu.Unlock()
	if l != nil && !retrySSH {
		return l, nil
	}
	// SSH first when the SuperStation is on the network (fast, no console noise, big files are quick)
	var herr error
	if s.wantSSH() {
		sl, err := s.openSSH()
		if err == nil {
			s.mu.Lock()
			if s.link != nil && s.link != Link(s.serial) {
				s.mu.Unlock()
				_ = sl.Close() // another call got there first
				return s.get()
			}
			if retrySSH {
				log.Printf("watchdog: SSH is back, using it again (the debug port stays the console)")
			}
			s.link = sl
			s.mu.Unlock()
			return sl, nil
		}
		herr = err
	}
	if l != nil {
		return l, nil // still on the console; SSH is tried again later
	}
	if !s.wantSerial() {
		return nil, herr
	}
	c, serr := s.console()
	if serr != nil {
		if herr == nil {
			return nil, serr
		}
		return nil, fmt.Errorf("ssh: %v; serial: %v", herr, serr)
	}
	s.mu.Lock()
	if s.link == nil {
		s.link = c
	}
	l = s.link
	s.mu.Unlock()
	return l, nil
}

// console: the debug port, opened if it isn't. Only one caller opens it at a time.
func (s *session) console() (*SerialLink, error) {
	if !s.wantSerial() {
		return nil, fmt.Errorf("%v (transport=ssh in cm-link.ini)", errNotSerial)
	}
	s.openMu.Lock()
	defer s.openMu.Unlock()
	s.mu.Lock()
	if s.serial != nil && s.serial.Alive() {
		c := s.serial
		s.mu.Unlock()
		return c, nil
	}
	if s.serial != nil {
		s.forget(s.serial)
	}
	cfg := s.cfg
	if s.port != "" && (cfg.Port == "" || cfg.Port == "auto") {
		cfg.Port = s.port // the port found before: no Enter typed into other devices
	}
	s.mu.Unlock()
	c, err := OpenSerial(cfg)
	if err != nil {
		return nil, fmt.Errorf("%v: %w", errNotSerial, err)
	}
	s.mu.Lock()
	s.serial, s.port = c, c.portName
	s.mu.Unlock()
	return c, nil
}

// forget (holding mu): close a link and clear it from wherever it's used.
func (s *session) forget(l Link) {
	_ = l.Close()
	if s.link == l {
		s.link = nil
	}
	if s.serial != nil && Link(s.serial) == l {
		s.serial = nil
	}
}

// drop forgets the main link after it failed, so the next call reconnects. The console stays
// unless it's the one that failed (its own checks say so).
func (s *session) drop() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.link == nil {
		return
	}
	if sl, ok := s.link.(*SerialLink); ok && sl.Alive() {
		s.link = nil // the console answers: keep it, just reconnect the main link (SSH first)
		return
	}
	s.forget(s.link)
}

func (s *session) dropConsole() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.serial != nil {
		s.forget(s.serial)
	}
}

func (s *session) close() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.link != nil {
		s.forget(s.link)
	}
	if s.serial != nil {
		s.forget(s.serial)
	}
}

// keep: the debug port stays open and checked for as long as CM Link runs. Left closed for a
// while, the dock's USB serial bridge can stop answering until it's unplugged (#119); held open and
// used now and then, it stays awake, and a console that stops answering is noticed and opened again
// instead of failing the next call that needs it.
func (s *session) keep() {
	if s.wantSerial() {
		s.keepGo.Do(func() { go s.keeper() })
	}
}

func (s *session) keeper() {
	fails := 0
	time.Sleep(5 * time.Second) // opened soon after start, so it never sits closed
	for ; ; time.Sleep(keepEvery) {
		s.mu.Lock()
		c := s.serial
		s.mu.Unlock()
		if c != nil && c.Check() {
			if fails > 0 {
				log.Printf("keeper: the debug port answers again")
			}
			fails = 0
			continue
		}
		if c != nil {
			log.Printf("keeper: the debug port stopped answering, opening it again")
			s.dropConsole()
		}
		if _, err := s.console(); err != nil {
			fails++
			if fails == 1 || fails%10 == 0 {
				log.Printf("keeper: the debug port isn't answering (%v): unplug the dock's debug cable and plug it in again if this goes on", err)
			}
			continue
		}
		if fails > 0 {
			log.Printf("keeper: the debug port answers again")
		}
		fails = 0
	}
}

var errNotSerial = errors.New("this needs the serial debug port (the dock's USB-C debug port connected to this PC)")

// keepEvery: how often the keeper checks the debug port (PL_KEEP_SECS for tests)
var keepEvery = func() time.Duration {
	if n, err := strconv.Atoi(os.Getenv("PL_KEEP_SECS")); err == nil && n > 0 {
		return time.Duration(n) * time.Second
	}
	return 90 * time.Second
}()
