// Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.

package main

// Settings in %APPDATA%\Console Mode\config.json: the SS1's address, the pairing key and the box-art
// passwords and keys (protected with Windows' own encryption for this user, see secret_windows.go),
// and the app's look.

import (
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
)

type config struct {
	Address  string `json:"address"`        // host:port of the SS1's remote access
	Name     string `json:"name,omitempty"` // what the SS1 calls itself
	DeviceID string `json:"device_id,omitempty"`
	TokenEnc string `json:"token,omitempty"`    // the pairing key, protected
	Accent   string `json:"accent"`             // amber, red, green, blue, purple
	Texture  int    `json:"texture"`            // the console's texture numbers
	QuietPC  bool   `json:"quiet_pc,omitempty"` // no Windows notifications (still listed in the app)
	// don't look for the SS1 on the network when it stops answering (reconnect.go; on by default)
	NoReconnect bool `json:"no_reconnect,omitempty"`
	// box-art logins for lookups on this PC (accounts.go): user names and IDs as they are,
	// passwords and keys protected like the pairing key
	Accounts    map[string]map[string]string `json:"accounts,omitempty"`
	AccountsEnc map[string]map[string]string `json:"accounts_protected,omitempty"`
	// a folder of box art on this PC, for the Local files source (scrape_local.go)
	ArtFolder string `json:"art_folder,omitempty"`
	// where every backup this app makes on the PC goes (backups.go); empty: Documents\Console Mode\Backups
	BackupsDir string `json:"backups_dir,omitempty"`
}

var (
	cfgMu sync.Mutex
	cfg   = config{Accent: "amber", Texture: 0}
)

func configDir() string {
	d, err := os.UserConfigDir()
	if err != nil {
		d = "."
	}
	return filepath.Join(d, configName)
}

func configPath() string { return filepath.Join(configDir(), "config.json") }

func loadConfig() {
	cfgMu.Lock()
	defer cfgMu.Unlock()
	if b, err := os.ReadFile(configPath()); err == nil {
		_ = json.Unmarshal(b, &cfg)
	}
	if cfg.Accent == "" {
		cfg.Accent = "amber"
	}
}

// saveConfig writes the settings (callers hold no lock)
func saveConfig() error {
	cfgMu.Lock()
	b, _ := json.MarshalIndent(cfg, "", "  ")
	cfgMu.Unlock()
	if err := os.MkdirAll(configDir(), 0o700); err != nil {
		return err
	}
	tmp := configPath() + ".tmp"
	if err := os.WriteFile(tmp, b, 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, configPath())
}

func token() string {
	cfgMu.Lock()
	enc := cfg.TokenEnc
	cfgMu.Unlock()
	if enc == "" {
		return ""
	}
	t, err := unprotect(enc)
	if err != nil {
		return ""
	}
	return t
}

func setPairing(addr, name, id, tok string) error {
	enc := ""
	if tok != "" {
		var err error
		if enc, err = protect(tok); err != nil {
			return err
		}
	}
	cfgMu.Lock()
	cfg.Address, cfg.Name, cfg.DeviceID, cfg.TokenEnc = addr, name, id, enc
	cfgMu.Unlock()
	return saveConfig()
}

func instancePath() string { return filepath.Join(configDir(), "instance.txt") }

func saveInstance(url string) {
	_ = os.MkdirAll(configDir(), 0o700)
	_ = os.WriteFile(instancePath(), []byte(url), 0o600)
}
