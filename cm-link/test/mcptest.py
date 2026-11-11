# Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see ../../LICENSE.md).
import json, subprocess, sys, time
p = subprocess.Popen(["./dist/cm-link-linux", "mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open("mcp.err","w"), text=True)
n = 0
def call(method, params=None, notify=False):
    global n
    msg = {"jsonrpc": "2.0", "method": method}
    if params is not None: msg["params"] = params
    if not notify:
        n += 1; msg["id"] = n
    p.stdin.write(json.dumps(msg) + "\n"); p.stdin.flush()
    if notify: return None
    r = json.loads(p.stdout.readline())
    assert r["id"] == n, r
    return r
r = call("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "t", "version": "1"}})
print("init:", r["result"]["protocolVersion"], r["result"]["serverInfo"])
call("notifications/initialized", notify=True)
print("tools:", [t["name"] for t in call("tools/list")["result"]["tools"]])
def tool(name, **a):
    t = time.time()
    r = call("tools/call", {"name": name, "arguments": a})["result"]
    print(f"--- {name} ({time.time()-t:.1f}s) isError={r['isError']}\n" + r["content"][0]["text"][:600])
tool("status")
tool("run", command="for i in 1 2 3; do echo line $i; done; ls /nonexistent", timeout_s=30)
tool("push_text", text="#!/bin/sh\necho hi from script\n", remote_path="/tmp/pltest/a.sh", mode="755")
tool("run", command="/tmp/pltest/a.sh")
tool("read_file", remote_path="/tmp/pltest/a.sh")
tool("pull_file", remote_path="/etc/os-release", local_path="pulled/os-release")
tool("read_file", remote_path="/nope")
tool("serial_log", seconds=2)
tool("recent_output", max_bytes=300)
tool("reboot", timeout_s=60)
tool("run", command="echo after reboot")
tool("disconnect")
print("ping:", call("ping"))
p.stdin.close(); p.wait(timeout=20); print("exit", p.returncode)
