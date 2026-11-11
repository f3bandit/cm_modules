// Adding CM Link to the Claude desktop app (claude_desktop_config.json).
// Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see ../LICENSE.md).
package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"time"
)

const serverName = "cm-link"

func claudeConfigPath() (string, error) {
	if p := os.Getenv("CMOD_LINK_CLAUDE_CONFIG"); p != "" {
		return p, nil // for testing
	}
	switch runtime.GOOS {
	case "windows":
		// the Microsoft Store version keeps its settings in its own package folder
		if la := os.Getenv("LOCALAPPDATA"); la != "" {
			if m, _ := filepath.Glob(filepath.Join(la, "Packages", "Claude_*", "LocalCache", "Roaming", "Claude")); len(m) == 1 {
				return filepath.Join(m[0], "claude_desktop_config.json"), nil
			}
		}
		ad := os.Getenv("APPDATA")
		if ad == "" {
			return "", errors.New("APPDATA isn't set")
		}
		return filepath.Join(ad, "Claude", "claude_desktop_config.json"), nil
	case "darwin":
		h, err := os.UserHomeDir()
		if err != nil {
			return "", err
		}
		return filepath.Join(h, "Library", "Application Support", "Claude", "claude_desktop_config.json"), nil
	default:
		d, err := os.UserConfigDir()
		if err != nil {
			return "", err
		}
		return filepath.Join(d, "Claude", "claude_desktop_config.json"), nil
	}
}

func loadClaudeConfig(p string) (map[string]any, []byte, error) {
	raw, err := os.ReadFile(p)
	if errors.Is(err, os.ErrNotExist) {
		return map[string]any{}, nil, nil
	}
	if err != nil {
		return nil, nil, err
	}
	cfg := map[string]any{}
	if len(raw) > 0 {
		if err := json.Unmarshal(raw, &cfg); err != nil {
			return nil, nil, fmt.Errorf("%s isn't valid JSON, so it was left alone: %v", p, err)
		}
	}
	return cfg, raw, nil
}

func saveClaudeConfig(p string, cfg map[string]any, old []byte) (string, error) {
	backup := ""
	if old != nil {
		backup = p + ".backup-" + time.Now().Format("20060102-150405")
		for i := 2; ; i++ {
			if _, err := os.Stat(backup); errors.Is(err, os.ErrNotExist) {
				break
			}
			backup = fmt.Sprintf("%s.backup-%s-%d", p, time.Now().Format("20060102-150405"), i)
		}
		if err := os.WriteFile(backup, old, 0644); err != nil {
			return "", fmt.Errorf("couldn't make a backup, nothing changed: %v", err)
		}
	}
	out, _ := json.MarshalIndent(cfg, "", "  ")
	if err := os.MkdirAll(filepath.Dir(p), 0755); err != nil {
		return "", err
	}
	tmp := p + ".tmp"
	if err := os.WriteFile(tmp, append(out, '\n'), 0644); err != nil {
		return "", err
	}
	return backup, os.Rename(tmp, p)
}

func installClaude() error {
	p, err := claudeConfigPath()
	if err != nil {
		return err
	}
	cfg, old, err := loadClaudeConfig(p)
	if err != nil {
		return err
	}
	exe, err := os.Executable()
	if err != nil {
		return err
	}
	if r, err := filepath.EvalSymlinks(exe); err == nil {
		exe = r
	}
	servers, _ := cfg["mcpServers"].(map[string]any)
	if servers == nil {
		servers = map[string]any{}
	}
	servers[serverName] = map[string]any{"command": exe, "args": []string{"mcp"}}
	cfg["mcpServers"] = servers
	backup, err := saveClaudeConfig(p, cfg, old)
	if err != nil {
		return err
	}
	fmt.Println("Added CM Link to the Claude desktop app:", p)
	if backup != "" {
		fmt.Println("Previous settings saved as", backup)
	}
	fmt.Println("Quit Claude completely (from the tray / menu bar too) and open it again to load it.")
	return nil
}

func uninstallClaude() error {
	p, err := claudeConfigPath()
	if err != nil {
		return err
	}
	cfg, old, err := loadClaudeConfig(p)
	if err != nil {
		return err
	}
	servers, _ := cfg["mcpServers"].(map[string]any)
	if _, ok := servers[serverName]; !ok {
		fmt.Println("CM Link isn't in the Claude desktop app's settings; nothing changed.")
		return nil
	}
	delete(servers, serverName)
	backup, err := saveClaudeConfig(p, cfg, old)
	if err != nil {
		return err
	}
	fmt.Println("Removed CM Link from the Claude desktop app. Previous settings saved as", backup)
	return nil
}
