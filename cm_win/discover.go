// Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.

package main

// Finding the SS1: GET /info on port 8767 of every address on this PC's home networks (/24 around
// each of its IPv4 addresses), a few dozen at a time, short timeouts. A couple of seconds in all.

import (
	"net"
	"sort"
	"sync"
	"time"
)

type found struct {
	Address string `json:"address"`
	Name    string `json:"name"`
	Version string `json:"version"`
}

// every IPv4 network this PC is on, private range or not (some home networks use other ranges);
// the sweep is the /24 around the PC's own address either way
func homeNet(ip net.IP) bool {
	ip = ip.To4()
	return ip != nil && !ip.IsLoopback() && !(ip[0] == 169 && ip[1] == 254)
}

func discover() []found {
	var targets []string
	seen := map[string]bool{}
	ifs, _ := net.Interfaces()
	for _, i := range ifs {
		if i.Flags&net.FlagUp == 0 || i.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, _ := i.Addrs()
		for _, a := range addrs {
			n, ok := a.(*net.IPNet)
			if !ok || !homeNet(n.IP) {
				continue
			}
			ip := n.IP.To4()
			for h := 1; h < 255; h++ {
				t := net.IPv4(ip[0], ip[1], ip[2], byte(h)).String()
				if !seen[t] && t != ip.String() {
					seen[t] = true
					targets = append(targets, t)
				}
			}
		}
	}
	var (
		mu  sync.Mutex
		out = []found{}
		wg  sync.WaitGroup
	)
	sem := make(chan struct{}, 64)
	for _, t := range targets {
		wg.Add(1)
		sem <- struct{}{}
		go func(t string) {
			defer wg.Done()
			defer func() { <-sem }()
			c, err := net.DialTimeout("tcp", t+":"+remotePort, 350*time.Millisecond)
			if err != nil {
				return
			}
			c.Close()
			if in, err := getInfo(t, 2*time.Second); err == nil {
				mu.Lock()
				out = append(out, found{Address: t + ":" + remotePort, Name: in.Name, Version: in.Version})
				mu.Unlock()
			}
		}(t)
	}
	wg.Wait()
	sort.Slice(out, func(i, j int) bool { return out[i].Address < out[j].Address })
	return out
}
