// SS1 Link: the MiSTer's serial console (the SuperStation One dock's USB-C UART port).
// Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
//
// The dock's debug port is a USB-to-serial bridge to the DE10-Nano's console UART (115200 8N1):
// U-Boot, the kernel, MiSTer Main's own output and a root login all come out there.
//
// Rules this follows:
//   - Never type while the MiSTer is booting: a key press in U-Boot's first seconds stops the boot.
//     It listens until the console has been quiet, or until "login:" if it saw a boot starting.
//   - Main prints to the same console at any moment, so command output never comes back raw:
//     it's saved to a file on the MiSTer and fetched as numbered, length-checked base64 lines
//     (anything Main prints in between is ignored, damaged lines are fetched again), then the
//     whole thing is checked with md5.
//   - Kernel messages on the console are muted while connected (dmesg -n 1) and put back after.
package main

import (
	"encoding/base64"
	"errors"
	"fmt"
	"os"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"go.bug.st/serial"
	"go.bug.st/serial/enumerator"
)

type SerialLink struct {
	cfg      Config
	portName string
	port     serial.Port

	mu      sync.Mutex
	buf     []byte // everything received; trimmed to keep the newest
	base    int    // absolute offset of buf[0]
	lastRx  time.Time
	closed  bool
	readErr error
	logf    *os.File
	opMu    sync.Mutex // one operation at a time
	dead    bool       // the watchdog gave up on this console: reopen the port
}

const keepBytes = 1 << 20

var ansiRe = regexp.MustCompile(`\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-Z0-9]|\r`)

func cleanText(s string) string { return ansiRe.ReplaceAllString(s, "") }

// ---------- finding the port ----------

type PortInfo struct {
	Name, VID, PID, Serial, Product string
	USB                             bool
}

func listPorts() ([]PortInfo, error) {
	var out []PortInfo
	det, err := enumerator.GetDetailedPortsList()
	if err == nil {
		for _, p := range det {
			out = append(out, PortInfo{Name: p.Name, VID: p.VID, PID: p.PID, Serial: p.SerialNumber, Product: p.Product, USB: p.IsUSB})
		}
		return out, nil
	}
	names, err2 := serial.GetPortsList()
	if err2 != nil {
		return nil, err
	}
	for _, n := range names {
		out = append(out, PortInfo{Name: n})
	}
	return out, nil
}

var bootRe = regexp.MustCompile(`U-Boot|Hit any key|Starting kernel|Booting Linux|Uncompressing|Freeing unused kernel`)
var promptRe = regexp.MustCompile(`(?:login: ?|Password: ?|[#$] ?|=> ?)$`)

// probe: does a MiSTer console answer on this port? Listens first, then presses Enter once.
func probe(name string, baud int) bool {
	p, err := serial.Open(name, &serial.Mode{BaudRate: baud})
	if err != nil && portBusy(err) && freeStale() > 0 {
		p, err = serial.Open(name, &serial.Mode{BaudRate: baud})
	}
	if err != nil {
		return false
	}
	defer p.Close()
	_ = p.SetReadTimeout(100 * time.Millisecond)
	var got []byte
	read := func(d time.Duration) {
		end := time.Now().Add(d)
		b := make([]byte, 512)
		for time.Now().Before(end) {
			n, _ := p.Read(b)
			got = append(got, b[:n]...)
		}
	}
	read(1500 * time.Millisecond)
	if bootRe.Match(got) {
		return true // it's booting: that's a MiSTer console, and we must not type now
	}
	_, _ = p.Write([]byte("\r"))
	read(2 * time.Second)
	if promptRe.MatchString(strings.TrimRight(cleanText(string(got)), " \n")+" ") || promptRe.MatchString(cleanText(string(got))) {
		return true
	}
	// a shell an earlier SS1 Link left logged in prints no prompt (PS1='', no echo): ask it
	got = nil
	_, _ = p.Write([]byte("echo PLPROBE_ok\r"))
	read(2 * time.Second)
	return strings.Contains(string(got), "PLPROBE_ok") || promptRe.MatchString(cleanText(string(got)))
}

func findPort(cfg Config) (string, error) {
	if cfg.Port != "" && cfg.Port != "auto" {
		return cfg.Port, nil
	}
	ports, err := listPorts()
	if err != nil {
		return "", err
	}
	var tried []string
	for _, p := range ports {
		if !p.USB && len(ports) > 1 {
			continue
		}
		tried = append(tried, p.Name)
		if probe(p.Name, cfg.Baud) {
			return p.Name, nil
		}
	}
	if len(tried) == 0 {
		return "", errors.New("no USB serial ports found: is the dock's USB-C debug port connected to this PC?")
	}
	return "", fmt.Errorf("no MiSTer console answered on %s", strings.Join(tried, ", "))
}

// ---------- connection ----------

// openRetry: Windows takes a moment to free a port after the last program closed it.
func openRetry(name string, baud int) (serial.Port, error) {
	var err error
	for i := 0; i < 15; i++ {
		var p serial.Port
		if p, err = serial.Open(name, &serial.Mode{BaudRate: baud}); err == nil {
			return p, nil
		}
		if !strings.Contains(strings.ToLower(err.Error()), "busy") && !strings.Contains(strings.ToLower(err.Error()), "access") {
			return nil, err
		}
		time.Sleep(300 * time.Millisecond)
	}
	// one last thing: an old copy of SS1 Link may still hold it
	if freeStale() > 0 {
		if p, err2 := serial.Open(name, &serial.Mode{BaudRate: baud}); err2 == nil {
			return p, nil
		}
	}
	return nil, fmt.Errorf("%v (another program may have it open, such as a terminal or the Claude app)", err)
}

func portBusy(err error) bool {
	e := strings.ToLower(err.Error())
	return strings.Contains(e, "busy") || strings.Contains(e, "access")
}

func OpenSerial(cfg Config) (*SerialLink, error) {
	name, err := findPort(cfg)
	if err != nil {
		return nil, err
	}
	p, err := openRetry(name, cfg.Baud)
	if err != nil {
		return nil, fmt.Errorf("can't open %s: %w", name, err)
	}
	_ = p.SetReadTimeout(200 * time.Millisecond)
	s := &SerialLink{cfg: cfg, portName: name, port: p, lastRx: time.Now()}
	if cfg.LogFile != "" {
		s.logf, _ = os.OpenFile(cfg.LogFile, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644)
		if s.logf != nil {
			fmt.Fprintf(s.logf, "\n==== %s connected to %s ====\n", time.Now().Format("2006-01-02 15:04:05"), name)
		}
	}
	go s.reader()
	if err := s.login(); err != nil {
		s.Close()
		return nil, err
	}
	return s, nil
}

func (s *SerialLink) Name() string { return "serial " + s.portName }

func (s *SerialLink) reader() {
	b := make([]byte, 4096)
	for {
		n, err := s.port.Read(b)
		s.mu.Lock()
		if s.closed {
			s.mu.Unlock()
			return
		}
		if n > 0 {
			s.buf = append(s.buf, b[:n]...)
			s.lastRx = time.Now()
			if len(s.buf) > keepBytes*2 {
				cut := len(s.buf) - keepBytes
				s.buf = append([]byte(nil), s.buf[cut:]...)
				s.base += cut
			}
			if s.logf != nil {
				_, _ = s.logf.Write(b[:n])
			}
		}
		if err != nil {
			s.readErr = err
			s.mu.Unlock()
			return
		}
		s.mu.Unlock()
	}
}

func (s *SerialLink) pos() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.base + len(s.buf)
}

func (s *SerialLink) since(from int) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	i := from - s.base
	if i < 0 {
		i = 0
	}
	if i > len(s.buf) {
		i = len(s.buf)
	}
	return string(s.buf[i:])
}

func (s *SerialLink) quietFor() time.Duration {
	s.mu.Lock()
	defer s.mu.Unlock()
	return time.Since(s.lastRx)
}

func (s *SerialLink) write(str string) error {
	_, err := s.port.Write([]byte(str))
	return err
}

// waitFor: text received since `from` matching re (on cleaned text), or timeout.
func (s *SerialLink) waitFor(from int, re *regexp.Regexp, timeout time.Duration) ([]string, string, error) {
	end := time.Now().Add(timeout)
	for {
		txt := cleanText(s.since(from))
		if m := re.FindStringSubmatch(txt); m != nil {
			return m, txt, nil
		}
		s.mu.Lock()
		rerr := s.readErr
		s.mu.Unlock()
		if rerr != nil {
			return nil, txt, fmt.Errorf("serial port: %w", rerr)
		}
		if time.Now().After(end) {
			return nil, txt, errors.New("timed out")
		}
		time.Sleep(20 * time.Millisecond)
	}
}

// settle: don't type during a boot. Wait for quiet; if a boot is under way, for the login prompt.
func (s *SerialLink) settle() {
	start := s.pos() - 4096
	for s.quietFor() < 1500*time.Millisecond {
		time.Sleep(100 * time.Millisecond)
	}
	if bootRe.MatchString(s.since(start)) && !strings.Contains(s.since(start), "login:") {
		// until the login prompt, or U-Boot's prompt if the boot was stopped (login() handles that)
		_, _, _ = s.waitFor(start, regexp.MustCompile(`login:|=> ?$`), 180*time.Second)
		for s.quietFor() < time.Second {
			time.Sleep(100 * time.Millisecond)
		}
	}
}

var loginRe = regexp.MustCompile(`(login: ?|Password: ?|[#$] ?|=> ?)\s*$`)

func (s *SerialLink) login() error {
	s.settle()
	for attempt := 0; attempt < 6; attempt++ {
		from := s.pos()
		_ = s.write("\r")
		m, _, err := s.waitFor(from, loginRe, 4*time.Second)
		if err != nil {
			// no prompt: maybe a shell an earlier SS1 Link left logged in (it prints none)
			if s.ping(3 * time.Second) {
				return s.setup()
			}
			continue
		}
		switch strings.TrimSpace(m[1]) {
		case "=>":
			// U-Boot's prompt: the boot was stopped (not by us); let it carry on
			from = s.pos()
			_ = s.write("boot\r")
			_, _, _ = s.waitFor(from, regexp.MustCompile(`login:`), 180*time.Second)
			time.Sleep(time.Second)
			continue
		case "login:":
			from = s.pos()
			_ = s.write(s.cfg.User + "\r")
			m2, _, err := s.waitFor(from, regexp.MustCompile(`(Password: ?|[#$] ?)\s*$`), 5*time.Second)
			if err != nil {
				continue
			}
			if strings.HasPrefix(m2[1], "Password") {
				from = s.pos()
				_ = s.write(s.cfg.Password + "\r")
				m3, _, err := s.waitFor(from, regexp.MustCompile(`(Login incorrect|[#$] ?\s*$)`), 10*time.Second)
				if err != nil {
					continue
				}
				if m3[1] == "Login incorrect" {
					return errors.New("login refused: check user and password in ss1-link.ini")
				}
			}
		case "Password:":
			continue // half-way through someone else's login: Enter resets it
		}
		return s.setup()
	}
	return errors.New("no shell prompt on the serial console")
}

func (s *SerialLink) setup() error {
	_ = s.write("stty -echo -onlcr; export PS1='' PS2='' TERM=dumb HISTFILE=/dev/null; rm -f /tmp/pl.k.*; " +
		"[ -f /tmp/pl.printk ] || cut -f1 /proc/sys/kernel/printk > /tmp/pl.printk; dmesg -n 1\r")
	// Main's output can break up the answer, so ask more than once
	for try := 0; try < 4; try++ {
		id := newID()
		from := s.pos()
		_ = s.write("echo PLREADY_" + id + "\r")
		if _, _, err := s.waitFor(from, regexp.MustCompile(`(?m)^PLREADY_`+id+`$`), 4*time.Second); err == nil {
			time.Sleep(100 * time.Millisecond)
			return nil
		}
	}
	return errors.New("shell didn't answer")
}

// line types one command line, then waits for its marker line. The marker (an echo argument,
// evaluated once on the MiSTer) is also saved to a file, so when Main's output breaks the marker
// line up, asking for the file again gives the answer without running the command twice.
func (s *SerialLink) line(body, marker, re string, timeout time.Duration) ([]string, string, error) {
	key := newID()
	f := "/tmp/pl.k." + key
	full := regexp.MustCompile(`(?m)^(?:` + re + `)$`)
	from := s.pos()
	if err := s.write(body + "; echo " + marker + " > " + f + "; cat " + f + "\r"); err != nil {
		return nil, "", err
	}
	end := time.Now().Add(timeout)
	lastAsk := time.Now()
	asked, answered := 0, true
	for {
		txt := cleanText(s.since(from))
		if m := full.FindStringSubmatch(txt); m != nil {
			return m, txt, nil
		}
		if asked > 0 && strings.Contains(txt, fmt.Sprintf("PLW_%s_%d\n", key, asked)) {
			answered = true
		}
		s.mu.Lock()
		rerr := s.readErr
		s.mu.Unlock()
		if rerr != nil {
			return nil, txt, fmt.Errorf("serial port: %w", rerr)
		}
		if time.Now().After(end) {
			return nil, txt, errors.New("timed out")
		}
		// No answer yet: ask for the saved marker. A question typed while the command still runs
		// waits in line and is answered when it finishes, so only one is outstanding at a time.
		since := time.Since(lastAsk)
		if (answered && since > 3*time.Second) || since > 20*time.Second {
			asked++
			answered = false
			lastAsk = time.Now()
			_ = s.write(fmt.Sprintf("if [ -f %s ]; then cat %s; else echo PLW_%s_%d; fi\r", f, f, key, asked))
		}
		time.Sleep(20 * time.Millisecond)
	}
}

func sq(s string) string { return "'" + strings.ReplaceAll(s, "'", `'\''`) + "'" }

// ---------- files ----------

func (s *SerialLink) push(data []byte, remote, mode string) error {
	id := newID()
	tmp := "/tmp/pl." + id + ".b64"
	if _, _, err := s.line(": > "+tmp, "PLA_"+id+"_0", "PLA_"+id+"_0", 10*time.Second); err != nil {
		return err
	}
	enc := base64.StdEncoding.EncodeToString(data)
	const chunk = 1024
	for i, n := 0, 1; i < len(enc); i, n = i+chunk, n+1 {
		j := i + chunk
		if j > len(enc) {
			j = len(enc)
		}
		ack := fmt.Sprintf("PLA_%s_%d", id, n)
		if _, _, err := s.line("printf '%s\\n' '"+enc[i:j]+"' >> "+tmp, ack, ack, 30*time.Second); err != nil {
			return fmt.Errorf("sending part %d: %w", n, err)
		}
	}
	if mode == "" {
		mode = "644"
	}
	r := sq(remote)
	m, _, err := s.line("mkdir -p \"$(dirname "+r+")\" && base64 -d "+tmp+" > "+r+".pltmp && mv -f "+r+".pltmp "+r+" && chmod "+mode+" "+r+
		"; rm -f "+tmp, "PLM_"+id+" $(md5sum < "+r+" 2>/dev/null | cut -c1-32)", "PLM_"+id+` ([0-9a-f]{32})?`, 120*time.Second)
	if err != nil {
		return fmt.Errorf("writing %s: %w", remote, err)
	}
	if m[1] == "" {
		return fmt.Errorf("couldn't write %s", remote)
	}
	if m[1] != md5hex(data) {
		return fmt.Errorf("%s arrived damaged (md5 %s, expected %s)", remote, m[1], md5hex(data))
	}
	return nil
}

func (s *SerialLink) pull(remote string) ([]byte, error) {
	id := newID()
	r := sq(remote)
	m, _, err := s.line(":", `$([ -r `+r+` ] && echo "PLE_`+id+` $(base64 `+r+` | wc -l) $(md5sum < `+r+` | cut -c1-32)" || echo PLX_`+id+`)`,
		`PLE_`+id+` (\d+) ([0-9a-f]{32})|PLX_`+id, 120*time.Second)
	if err != nil {
		return nil, fmt.Errorf("reading %s: %w", remote, err)
	}
	if m[1] == "" {
		return nil, fmt.Errorf("%s doesn't exist or can't be read", remote)
	}
	total, _ := strconv.Atoi(m[1])
	want := m[2]
	lines := map[int]string{}
	collect := func(t string, p string) {
		re := regexp.MustCompile(`(?m)^` + p + ` (\d+) (\d+) ([A-Za-z0-9+/=]*)$`)
		for _, x := range re.FindAllStringSubmatch(t, -1) {
			n, _ := strconv.Atoi(x[1])
			l, _ := strconv.Atoi(x[2])
			if len(x[3]) == l && n >= 1 && n <= total {
				lines[n] = x[3]
			}
		}
	}
	if total > 0 {
		pre := "PLD_" + id
		_, txt, err := s.line("base64 "+r+" | awk '{ print \""+pre+" \" NR \" \" length($0) \" \" $0 }'", "PLF_"+id, "PLF_"+id, 600*time.Second)
		if err != nil {
			return nil, fmt.Errorf("reading %s: %w", remote, err)
		}
		collect(txt, pre)
	}
	// lines Main's output broke up: fetch those again, a batch at a time
	for try := 0; try < 10; try++ {
		var missing []string
		for n := 1; n <= total && len(missing) < 200; n++ {
			if _, ok := lines[n]; !ok {
				missing = append(missing, strconv.Itoa(n))
			}
		}
		if len(missing) == 0 {
			break
		}
		id2 := newID()
		pre2 := "PLD_" + id2
		_, t2, err := s.line("base64 "+r+" | awk -v want="+strings.Join(missing, ",")+
			" 'BEGIN { n = split(want, w, \",\"); for (i = 1; i <= n; i++) k[w[i]] = 1 } (NR in k) { print \""+pre2+" \" NR \" \" length($0) \" \" $0 }'",
			"PLF_"+id2, "PLF_"+id2, 120*time.Second)
		if err == nil {
			collect(t2, pre2)
		}
	}
	if len(lines) != total {
		return nil, fmt.Errorf("couldn't read all of %s (%d of %d parts)", remote, len(lines), total)
	}
	var sb strings.Builder
	for n := 1; n <= total; n++ {
		sb.WriteString(lines[n])
	}
	data, err := base64.StdEncoding.DecodeString(sb.String())
	if err != nil {
		return nil, fmt.Errorf("decoding %s: %w", remote, err)
	}
	if md5hex(data) != want {
		return nil, fmt.Errorf("%s arrived damaged (md5 mismatch)", remote)
	}
	return data, nil
}

// ---------- Link ----------

func (s *SerialLink) cleanup(id string) {
	_, _, _ = s.line("rm -f /tmp/pl."+id+".* /tmp/pl.k.*", "PLC_"+id, "PLC_"+id, 10*time.Second)
}

func (s *SerialLink) Run(cmd string, timeout time.Duration) (string, int, error) {
	s.opMu.Lock()
	defer s.opMu.Unlock()
	id := newID()
	script := "/tmp/pl." + id + ".sh"
	out := "/tmp/pl." + id + ".out"
	if err := s.push([]byte(cmd+"\n"), script, "644"); err != nil {
		// no answer while sending the script: the console may have died silently (the debug port
		// stopped passing data, or the MiSTer rebooted under us). Check it, so a dead one is replaced.
		if strings.Contains(err.Error(), "timed out") && !s.revive() {
			return "", -1, fmt.Errorf("the console stopped answering (link reset)")
		}
		return "", -1, err
	}
	m, _, err := s.line("bash "+script+" > "+out+" 2>&1 < /dev/null", "PLRC_"+id+" $?", "PLRC_"+id+` (\d+)`, timeout)
	if err != nil {
		// watchdog: stop the command, then make sure the console still answers
		if !s.revive() {
			return "", -1, fmt.Errorf("command %s after %s, and the console stopped answering (link reset)", err, timeout)
		}
		partial, _ := s.pull(out)
		s.cleanup(id)
		return string(partial), -1, fmt.Errorf("command %s after %s", err, timeout)
	}
	rc, _ := strconv.Atoi(m[1])
	data, perr := s.pull(out)
	s.cleanup(id)
	return string(data), rc, perr
}

func (s *SerialLink) Push(data []byte, remote, mode string) error {
	s.opMu.Lock()
	defer s.opMu.Unlock()
	err := s.push(data, remote, mode)
	if err != nil && strings.Contains(err.Error(), "timed out") && !s.revive() {
		return fmt.Errorf("%v, and the console stopped answering (link reset)", err)
	}
	return err
}

func (s *SerialLink) Pull(remote string) ([]byte, error) {
	s.opMu.Lock()
	defer s.opMu.Unlock()
	data, err := s.pull(remote)
	if err != nil && strings.Contains(err.Error(), "timed out") && !s.revive() {
		return nil, fmt.Errorf("%v, and the console stopped answering (link reset)", err)
	}
	return data, err
}

// ping: does the shell answer an echo?
func (s *SerialLink) ping(wait time.Duration) bool {
	id := newID()
	from := s.pos()
	_ = s.write("echo PLPING_" + id + "\r")
	_, _, err := s.waitFor(from, regexp.MustCompile(`(?m)^PLPING_`+id+`$`), wait)
	return err == nil
}

// revive (called holding opMu): Ctrl-C whatever is stuck, then check the shell answers. If the
// MiSTer rebooted under us, log in again. Gives up (and marks the link dead) if nothing helps.
func (s *SerialLink) revive() bool {
	for try := 0; try < 3; try++ {
		_ = s.write("\x03")
		time.Sleep(300 * time.Millisecond)
		_ = s.write("\r")
		time.Sleep(200 * time.Millisecond)
		if s.ping(3 * time.Second) {
			return true
		}
		if strings.Contains(cleanText(s.Recent(400)), "login:") {
			if s.login() == nil {
				return true
			}
		}
	}
	s.mu.Lock()
	s.dead = true
	s.mu.Unlock()
	return false
}

// Check (the keeper's health check): when nothing else is using the console, does the shell still
// answer? A console that doesn't is revived, or marked dead so it's opened again. Busy counts as fine.
func (s *SerialLink) Check() bool {
	if !s.opMu.TryLock() {
		return true
	}
	defer s.opMu.Unlock()
	if !s.Alive() {
		return false
	}
	if s.ping(3 * time.Second) {
		return true
	}
	return s.revive()
}

// Alive (for the session's watchdog): false once the port failed or revive gave up. A link
// that's busy with another call counts as alive.
func (s *SerialLink) Alive() bool {
	s.mu.Lock()
	bad := s.closed || s.dead || s.readErr != nil
	s.mu.Unlock()
	return !bad
}

// Listen: capture what the console prints (boot log, Main's messages) without typing anything.
func (s *SerialLink) Listen(d time.Duration, until string) (string, error) {
	from := s.pos()
	if until != "" {
		_, txt, err := s.waitFor(from, regexp.MustCompile(regexp.QuoteMeta(until)), d)
		if err != nil && err.Error() != "timed out" {
			return txt, err
		}
		return txt, nil
	}
	time.Sleep(d)
	return cleanText(s.since(from)), nil
}

func (s *SerialLink) Recent(max int) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	b := s.buf
	if len(b) > max {
		b = b[len(b)-max:]
	}
	return cleanText(string(b))
}

// Reboot the MiSTer and capture the whole boot, without typing until the login prompt.
func (s *SerialLink) Reboot(timeout time.Duration) (string, error) {
	s.opMu.Lock()
	defer s.opMu.Unlock()
	from := s.pos()
	_ = s.write(safeReboot + "\r")
	_, txt, err := s.waitFor(from, regexp.MustCompile(`login:`), timeout)
	if err != nil {
		return txt, fmt.Errorf("no login prompt within %s", timeout)
	}
	time.Sleep(1500 * time.Millisecond)
	if err := s.login(); err != nil {
		return txt, fmt.Errorf("rebooted, but logging in again failed: %w", err)
	}
	return txt, nil
}

func (s *SerialLink) Close() error {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return nil
	}
	s.mu.Unlock()
	// put kernel messages and the terminal back the way they were, and log out
	_ = s.write("dmesg -n $(cat /tmp/pl.printk 2>/dev/null || echo 7); rm -f /tmp/pl.printk; stty echo onlcr; exit\r")
	time.Sleep(300 * time.Millisecond)
	s.mu.Lock()
	s.closed = true
	s.mu.Unlock()
	if s.logf != nil {
		s.logf.Close()
	}
	return s.port.Close()
}

// safeReboot: everything written to the drives first (with Phosphor's safe stop when Phosphor is installed),
// so a restart from here doesn't leave the SD card "not shut down safely"
const safeReboot = `[ -x /media/fat/phosphor/phosphor.sh ] && timeout 25 /media/fat/phosphor/phosphor.sh safe-stop > /dev/null 2>&1; sync; reboot`
