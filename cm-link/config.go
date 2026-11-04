// CM Link settings: cm-link.ini next to the program (all optional).
// Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md).
package main

import (
	"bufio"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

type Config struct {
	Transport   string // auto | serial | ssh
	Port        string // auto or COM5 / /dev/ttyUSB0
	Baud        int
	User        string
	Password    string
	Host        string // for SSH
	SSHUser     string
	SSHPassword string
	LogFile     string
}

func defaultConfig() Config {
	return Config{Transport: "auto", Port: "auto", Baud: 115200, User: "root", Password: "1", SSHUser: "root", SSHPassword: "1"}
}

func exeDir() string {
	exe, err := os.Executable()
	if err != nil {
		return "."
	}
	if r, err := filepath.EvalSymlinks(exe); err == nil {
		exe = r
	}
	return filepath.Dir(exe)
}

func loadConfig() Config {
	c := defaultConfig()
	c.LogFile = filepath.Join(exeDir(), "cm-link.log")
	f, err := os.Open(filepath.Join(exeDir(), "cm-link.ini"))
	if err != nil {
		return c
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || line[0] == ';' || line[0] == '#' || line[0] == '[' {
			continue
		}
		k, v, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		k, v = strings.ToLower(strings.TrimSpace(k)), strings.Trim(strings.TrimSpace(v), `"`)
		switch k {
		case "transport":
			c.Transport = v
		case "port":
			c.Port = v
		case "baud":
			if n, err := strconv.Atoi(v); err == nil {
				c.Baud = n
			}
		case "user":
			c.User = v
		case "password":
			c.Password = v
		case "host":
			c.Host = v
		case "ssh_user":
			c.SSHUser = v
		case "ssh_password":
			c.SSHPassword = v
		case "log":
			c.LogFile = v
		}
	}
	return c
}
