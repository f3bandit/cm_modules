/* Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../../LICENSE); Retro Remake: see ../../LICENSE-RETRO-REMAKE.md. */
/* The screens, laid out like Console Mode on the TV: a list on the left with a white bar on the chosen row,
   box art on the right, the breadcrumb, page count and button hints at the bottom. Keyboard (arrows,
   Enter, Esc, X), mouse (click to choose, click again or double-click to open, wheel, right-click back)
   or a controller plugged into the PC (A B X, the d-pad, LB/RB a page). */
"use strict";

/* ---------- talking to the app (and through it, the SS1) ---------- */
const KEY = (() => {
	const k = new URLSearchParams(location.search).get("k");
	try { if (k) sessionStorage.setItem("k", k); } catch (e) {}
	let s = k;
	try { s = k || sessionStorage.getItem("k"); } catch (e) {}
	history.replaceState(null, "", "/");
	return s || "";
})();
async function api(path, body) {
	const r = await fetch(path, {
		method: body === undefined ? "GET" : "POST",
		headers: { "X-App-Key": KEY, "Content-Type": "application/json" },
		body: body === undefined ? undefined : JSON.stringify(body),
	});
	if (!r.ok) throw new Error("The app answered " + r.status);
	return r.json();
}
/* one command on the SS1 (the app link) */
async function ss1(cmd) {
	const j = await api("/api/call", cmd);
	if (j.forgotten) { S.paired = false; replace(Connect); }
	if (!j.ok) throw new Error(j.error || "the SS1 didn't answer");
	return j;
}
const artURL = (id, v) => `/api/art/${id}?k=${encodeURIComponent(KEY)}${v ? "&v=" + v : ""}`;

/* ---------- helpers ---------- */
const $ = (sel, root = document) => root.querySelector(sel);
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const n = x => Number(x || 0).toLocaleString();
function h(html) { const t = document.createElement("template"); t.innerHTML = html.trim(); return t.content; }
let toastT;
function toast(msg) {
	const t = $("#toast");
	t.textContent = msg;
	t.hidden = false;
	clearTimeout(toastT);
	toastT = setTimeout(() => (t.hidden = true), 3500);
}

/* ---------- the TV's shape: 1280x720, scaled to the window ---------- */
function fit() {
	const s = Math.min(innerWidth / 1280, innerHeight / 720);
	$("#stage").style.transform = `translate(-50%, -50%) scale(${s})`;
}
addEventListener("resize", fit);
fit();

/* ---------- the status line ---------- */
const S = { paired: false, connected: false, status: null, fontLoaded: false };
const WIFI = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="M2 8.5a15 15 0 0 1 20 0"/><path d="M5.5 12a10 10 0 0 1 13 0"/><path d="M9 15.5a5 5 0 0 1 6 0"/><circle cx="12" cy="19" r="1.3" fill="currentColor"/></svg>`;
function clock() {
	const d = new Date();
	const mon = d.toLocaleString("en-US", { month: "short" }), day = String(d.getDate()).padStart(2, "0");
	$("#clock").textContent = `${mon} ${day} ${d.toLocaleTimeString("en-US", { hour: "numeric", minute: "2-digit" })}`;
}
setInterval(clock, 5000);
clock();
function showStatus() {
	const v = S.status && S.status.frontend_version;
	$("#title").textContent = "Console Mode" + (v ? " v" + v : "");
	const net = $("#net");
	net.innerHTML = WIFI;
	net.className = S.connected ? "" : "off";
	net.title = S.connected ? `Connected to ${S.address}` : S.paired ? "The SS1 isn't answering" : "Not paired with an SS1";
}
$("#net").addEventListener("click", () => push(S.paired ? Settings : Connect));
async function refreshState() {
	try {
		const st = await api("/api/state");
		Object.assign(S, { paired: st.paired, connected: !!st.connected, address: st.address, name: st.name, appVersion: st.app_version, appName: st.app_name });
		S.status = st.status || null;
		if (st.forgotten) S.paired = false;
	} catch (e) { S.connected = false; }
	showStatus();
	if (S.connected && !S.fontLoaded) loadFont();
}
/* Console Mode's own font, from the SS1 (Windows' Bahnschrift until then) */
async function loadFont() {
	S.fontLoaded = true;
	try {
		for (const [w, which] of [["400", "regular"], ["700", "bold"]]) {
			const f = new FontFace("CMtv", `url(/api/font/${which}?k=${encodeURIComponent(KEY)})`, { weight: w });
			await f.load();
			document.fonts.add(f);
		}
	} catch (e) {}
}

/* ---------- screens: a stack ---------- */
const stack = [];
function topScr() { return stack[stack.length - 1]; }
function show(scr) {
	const root = $("#screen");
	root.innerHTML = "";
	$("#crumb").textContent = scr.crumb || "";
	$("#page").textContent = "";
	scr.render(root);
	setHints(scr.hints || [["B", "Back"], ["A", "OK"]]);
}
function push(Scr, ...args) { stack.push(new Scr(...args)); show(topScr()); }
function replace(Scr, ...args) { stack.length = 0; push(Scr, ...args); }
function back() {
	if (stack.length > 1) { stack.pop(); show(topScr()); }
}
/* the hints: Console Mode's button pictures; each can be clicked */
let hintList = [];
function setHints(list) {
	hintList = list;
	$("#hints").innerHTML = list.map(([k, t]) => `<span class="hint" data-k="${esc(k)}" title="${esc(KEYS[k] || k)}"><span class="btn-ic${k.length > 1 ? " sq" : ""}">${esc(k)}</span>${esc(t)}</span>`).join("");
}
const KEYS = { A: "Enter", B: "Esc", X: "X", Y: "Y", SEL: "Tab" };
$("#hints").addEventListener("mousedown", e => e.preventDefault());
$("#hints").addEventListener("click", e => {
	const el = e.target.closest(".hint");
	if (el) act({ A: "a", B: "b", X: "x", Y: "y", SEL: "sel" }[el.dataset.k]);
});

/* ---------- a list, the way Console Mode draws one ---------- */
const ROW = 43, VISIBLE = 13;
class List {
	/* items: [{t: text, v: right-hand text, dim, input: {value, placeholder, type}, open(), focus()}] */
	constructor(root, items, opts = {}) {
		this.items = items;
		this.opts = opts;
		this.sel = Math.min(opts.sel || 0, Math.max(0, items.length - 1));
		this.top = 0;
		this.el = h(`<div class="list${opts.cls ? " " + opts.cls : ""}"></div>`).firstChild;
		if (opts.style) this.el.setAttribute("style", opts.style);
		root.appendChild(this.el);
		if (!items.length && opts.empty) root.append(h(`<div class="empty">${esc(opts.empty)}</div>`));
		this.draw();
		this.el.addEventListener("wheel", e => { e.preventDefault(); this.move(e.deltaY > 0 ? 3 : -3, true); }, { passive: false });
	}
	draw() {
		this.el.innerHTML = this.items.map((it, i) => it.input
			? `<div class="row field-row${i === this.sel ? " sel" : ""}" data-i="${i}">${it.label ? `<span class="lbl">${esc(it.label)}</span>` : ""}<input type="${it.input.type || "text"}" value="${esc(it.input.value || "")}" placeholder="${esc(it.input.placeholder || "")}" spellcheck="false"></div>`
			: `<div class="row${i === this.sel ? " sel" : ""}${it.dim ? " dim" : ""}" data-i="${i}">${it.label ? `<span class="lbl">${esc(it.label)}</span>` : ""}<span class="t">${esc(it.t)}</span>${it.v !== undefined && it.v !== "" ? `<span class="v">${esc(it.v)}</span>` : ""}</div>`).join("");
		this.el.querySelectorAll(".row").forEach(r => {
			const i = +r.dataset.i;
			r.addEventListener("mousedown", e => {
				if (e.button !== 0) return;
				if (i === this.sel && !this.items[i].input) { this.open(); return; }
				this.select(i);
			});
			r.addEventListener("dblclick", () => { if (!this.items[i].input) this.open(); });
		});
		this.el.querySelectorAll("input").forEach(inp => {
			const i = +inp.closest(".row").dataset.i;
			inp.addEventListener("input", () => { this.items[i].input.value = inp.value; if (this.items[i].change) this.items[i].change(inp.value); });
		});
		this.select(this.sel, true);
	}
	rowEl(i) { return this.el.children[i]; }
	select(i, first) {
		if (!this.items.length) { this.page(); return; }
		i = Math.max(0, Math.min(this.items.length - 1, i));
		const old = this.rowEl(this.sel);
		if (old) old.classList.remove("sel");
		this.sel = i;
		const r = this.rowEl(i);
		r.classList.add("sel");
		const vis = this.opts.visible || VISIBLE;
		if (i < this.top) this.top = i;
		if (i >= this.top + vis) this.top = i - vis + 1;
		this.el.scrollTop = this.top * ROW;
		const inp = r.querySelector("input");
		if (inp) inp.focus({ preventScroll: true });
		else if (document.activeElement && document.activeElement.matches("input, textarea")) document.activeElement.blur();
		this.page();
		const it = this.items[i];
		if (it.focus) it.focus(it, first);
		if (this.opts.onSelect) this.opts.onSelect(it, i);
	}
	page() { if (this.opts.count !== false) $("#page").textContent = this.items.length ? `${this.sel + 1} / ${this.items.length}` : ""; }
	/* up and down go round, as on the TV; a page (left/right, LB/RB) stops at the ends */
	move(d, clamp) {
		const k = this.items.length;
		if (!k) return;
		let i = this.sel + d;
		if (clamp || Math.abs(d) > 1) i = Math.max(0, Math.min(k - 1, i));
		else i = (i + k) % k;
		this.select(i);
	}
	open() { const it = this.items[this.sel]; if (it && it.open) it.open(it.input ? it.input.value : undefined); }
}

/* the right-hand panel: box art, or a card with a name */
function panel(root, cls = "panel") {
	const el = h(`<div class="${cls}"></div>`).firstChild;
	root.appendChild(el);
	return el;
}
function card(el, title, sub) {
	el.innerHTML = `<div class="card"><b>${esc(title)}</b>${sub ? `<small>${esc(sub)}</small>` : ""}</div>`;
}
/* box art, chosen a moment after the row (so running down a long list doesn't load every picture) */
let artT;
function artInto(el, g, title) {
	clearTimeout(artT);
	if (!g || !g.art) { card(el, title || (g && g.title) || ""); return; }
	el.innerHTML = "";
	artT = setTimeout(() => {
		const img = new Image();
		img.onload = () => { el.innerHTML = ""; el.appendChild(img); };
		img.onerror = () => card(el, title || g.title);
		img.src = artURL(g.id, g.artv);
	}, 120);
}

/* ---------- input: keys, mouse, controller ---------- */
function act(name) {
	const s = topScr();
	if (!s) return;
	if (s.act && s.act(name)) return;
	const l = s.list;
	switch (name) {
	case "up": l && l.move(-1); break;
	case "down": l && l.move(1); break;
	case "left": case "lb": l && l.move(-(l.opts.visible || VISIBLE)); break;
	case "right": case "rb": l && l.move(l.opts.visible || VISIBLE); break;
	case "a": l && l.open(); break;
	case "b": back(); break;
	case "x": s.x ? s.x() : push(Search); break;
	}
}
addEventListener("keydown", e => {
	const typing = document.activeElement && document.activeElement.matches("input, textarea");
	const k = e.key;
	if (document.activeElement && document.activeElement.matches("textarea") && k !== "Escape") return;
	if (typing && !["ArrowUp", "ArrowDown", "Escape", "Enter", "Tab"].includes(k)) return;
	const map = { ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right", PageUp: "lb", PageDown: "rb", Enter: "a", " ": "a", Escape: "b", Backspace: "b", x: "x", X: "x", y: "y", Y: "y", Tab: "sel" };
	const a = map[k];
	if (!a) return;
	e.preventDefault();
	act(a);
});
document.addEventListener("contextmenu", e => {
	if (e.target.closest("input, textarea")) return;
	e.preventDefault();
	act("b");
});
document.addEventListener("mouseup", e => { if (e.button === 3) { e.preventDefault(); act("b"); } });

const pad = { held: {}, next: {}, raf: 0 };
function padLoop() {
	pad.raf = 0;
	const gp = [...(navigator.getGamepads ? navigator.getGamepads() : [])].find(g => g && g.connected);
	if (!gp) return;
	if (document.hasFocus()) {
		const b = i => gp.buttons[i] && gp.buttons[i].pressed;
		const ax = gp.axes[0] || 0, ay = gp.axes[1] || 0;
		const st = { up: b(12) || ay < -0.6, down: b(13) || ay > 0.6, left: b(14) || ax < -0.6, right: b(15) || ax > 0.6, a: b(0), b: b(1), x: b(2), y: b(3), lb: b(4), rb: b(5), sel: b(8) };
		const now = performance.now();
		for (const k in st) {
			if (st[k] && !pad.held[k]) { act(k); pad.next[k] = now + 400; }
			else if (st[k] && ["up", "down", "left", "right"].includes(k) && now >= pad.next[k]) { act(k); pad.next[k] = now + 90; }
			pad.held[k] = st[k];
		}
	}
	pad.raf = requestAnimationFrame(padLoop);
}
addEventListener("gamepadconnected", () => { if (!pad.raf) pad.raf = requestAnimationFrame(padLoop); });

/* ---------- Connect: find the SS1, pair with the code Console Mode shows ---------- */
class Connect {
	constructor() { this.crumb = "Connect"; this.found = null; }
	render(root) {
		const items = [];
		if (this.found === null) items.push({ t: "Looking for your SuperStation…", dim: true });
		else {
			for (const f of this.found) items.push({ t: f.name === "Console Mode" ? `SuperStation at ${f.address.replace(/:8767$/, "")}` : `${f.name} at ${f.address.replace(/:8767$/, "")}`, open: () => push(Pair, f.address) });
			items.push({ t: "Look again", open: () => { this.found = null; show(this); this.look(); } });
		}
		items.push({ label: "Address", input: { value: "", placeholder: "Type the SS1's address" }, open: v => v && v.trim() && push(Pair, v.trim()) });
		this.list = new List(root, items, { sel: this.found && this.found.length ? 0 : items.length - 1 });
		root.append(h(`<div class="note">To use this PC with your SuperStation, turn on <b>Remote access</b> in Console Mode's settings, then choose your SuperStation here (or type the address Console Mode shows).<br><br>Then choose <b>Pair a device</b> on the TV and type the code it shows.</div>`));
		if (this.found === null) this.look();
	}
	async look() {
		try { this.found = (await api("/api/discover")).found || []; } catch (e) { this.found = []; }
		if (topScr() === this) show(this);
	}
}
class Pair {
	constructor(addr) { this.addr = addr; this.crumb = "Connect / Pair"; }
	render(root) {
		const items = [
			{ label: "Code", input: { value: "", placeholder: "The 6-digit code on the TV", type: "text" }, open: v => this.pair(v) },
			{ t: "Pair", open: () => this.pair(this.list.items[0].input.value) },
		];
		this.list = new List(root, items);
		root.append(h(`<div class="note">On the SuperStation: <b>Settings › Remote access › Pair a device</b>.<br><br>Console Mode shows a code for two minutes: type it here.<br><br>SuperStation: <b>${esc(this.addr)}</b></div>`));
	}
	async pair(code) {
		code = (code || "").replace(/\D/g, "");
		if (code.length !== 6) { toast("Type the 6 numbers the TV shows"); return; }
		try {
			const j = await api("/api/pair", { address: this.addr, code });
			if (!j.ok) { toast(j.error); return; }
			toast("Paired");
			await refreshState();
			replace(Home);
		} catch (e) { toast(e.message); }
	}
}

/* ---------- Home: Console Mode's main menu (the entries that work from a PC so far) ---------- */
class Home {
	constructor() { this.crumb = "Home"; this.sel = 0; this.hints = [["A", "OK"]]; }
	render(root) {
		const items = [
			{ t: "Load Game", open: () => push(Groups) },
			{ t: "History", open: () => push(GameList, "History", { filter: "recent" }) },
			{ t: "Favorites", open: () => push(GameList, "Favorites", { filter: "favorites" }) },
			{ t: "Settings", open: () => push(Settings) },
		];
		this.list = new List(root, items, { sel: this.sel, cls: "home", style: "left:203px;width:234px;text-align:center", count: false, onSelect: (it, i) => (this.sel = i) });
		this.list.el.querySelectorAll(".row .t").forEach(t => (t.style.textAlign = "center"));
	}
	act(a) { return a === "b"; }    /* Home has no Back */
}

/* ---------- Load Game: groups, systems, then a system's folders and games ---------- */
class Groups {
	constructor() { this.crumb = "Load Game"; this.sel = 0; }
	render(root) {
		this.p = panel(root);
		this.list = new List(root, [{ t: "Loading…", dim: true }], { count: false });
		this.load(root);
	}
	async load(root) {
		try {
			const j = await ss1({ cmd: "groups" });
			if (topScr() !== this) return;
			const items = j.groups.map(g => ({ t: g.name, focus: () => card(this.p, g.name, `${n(g.systems)} system${g.systems === 1 ? "" : "s"} · ${n(g.games)} games`), open: () => push(Systems, g.name) }));
			root.querySelector(".list").remove();
			this.list = new List(root, items, { sel: this.sel, onSelect: (it, i) => (this.sel = i), empty: "No games found" });
		} catch (e) { toast(e.message); }
	}
}
class Systems {
	constructor(group) { this.group = group; this.crumb = `Load Game / ${group}`; this.sel = 0; }
	render(root) {
		this.p = panel(root);
		this.list = new List(root, [{ t: "Loading…", dim: true }], { count: false });
		this.load(root);
	}
	async load(root) {
		try {
			const j = await ss1({ cmd: "systems", group: this.group });
			if (topScr() !== this) return;
			const items = j.systems.map(s => ({ t: s.name, focus: () => card(this.p, s.name, `${n(s.games)} game${s.games === 1 ? "" : "s"}`), open: () => push(Browse, this.group, s.name, "") }));
			root.querySelector(".list").remove();
			this.list = new List(root, items, { sel: this.sel, onSelect: (it, i) => (this.sel = i) });
		} catch (e) { toast(e.message); }
	}
}
class Browse {
	constructor(group, sys, path) {
		this.group = group; this.sys = sys; this.path = path; this.sel = 0;
		this.crumb = [group, sys, ...(path ? path.split("/") : [])].join(" / ");
		this.hints = [["X", "Search"], ["B", "Back"], ["A", "OK"]];
	}
	render(root) {
		this.p = panel(root);
		this.list = new List(root, [{ t: "Loading…", dim: true }], { count: false });
		this.load(root);
	}
	async load(root) {
		try {
			const j = this.data || await ss1({ cmd: "browse", system: this.sys, path: this.path });
			this.data = j;
			if (topScr() !== this) return;
			const items = j.entries.map(e => e.type === "dir"
				? { t: `${e.title} [DIR]`, focus: () => card(this.p, e.title, `${n(e.games)} game${e.games === 1 ? "" : "s"}`), open: () => push(Browse, this.group, this.sys, e.path) }
				: { t: e.title, g: e, focus: () => artInto(this.p, e), open: () => play(e) });
			root.querySelector(".list").remove();
			this.list = new List(root, items, { sel: this.sel, onSelect: (it, i) => (this.sel = i), empty: "Nothing here" });
		} catch (e) { toast(e.message); }
	}
}

/* a list of games from a filter or a search (History, Favorites) */
class GameList {
	constructor(title, query) { this.title = title; this.query = query; this.crumb = title; this.sel = 0; this.hints = [["X", "Search"], ["B", "Back"], ["A", "OK"]]; }
	render(root) {
		this.p = panel(root);
		this.list = new List(root, [{ t: "Loading…", dim: true }], { count: false });
		this.load(root);
	}
	async load(root) {
		try {
			const j = this.data || await ss1(Object.assign({ cmd: "games", limit: 5000 }, this.query));
			this.data = j;
			if (topScr() !== this) return;
			const items = j.games.map(g => ({ t: g.title, focus: () => artInto(this.p, g), open: () => play(g) }));
			root.querySelector(".list").remove();
			this.list = new List(root, items, { sel: this.sel, onSelect: (it, i) => (this.sel = i), empty: "No games here" });
		} catch (e) { toast(e.message); }
	}
}

/* ---------- Search: every system ---------- */
class Search {
	constructor() { this.crumb = "Search"; this.q = ""; this.games = []; this.sel = 0; this.hints = [["B", "Back"], ["A", "OK"]]; }
	render(root) {
		this.root = root;
		this.p = panel(root);
		this.draw();
	}
	draw() {
		const old = this.root.querySelector(".list");
		if (old) old.remove();
		const old2 = this.root.querySelector(".empty");
		if (old2) old2.remove();
		const items = [{ label: "Search", input: { value: this.q, placeholder: "Part of a game's name" }, change: v => this.typed(v), open: () => this.list.move(1) }]
			.concat(this.games.map(g => ({ t: g.title, focus: () => artInto(this.p, g), open: () => play(g) })));
		this.list = new List(this.root, items, { sel: this.sel, onSelect: (it, i) => { this.sel = i; if (!i) card(this.p, this.q || "Search", this.q ? `${n(this.games.length)} found` : "Type part of a name"); } });
		if (this.sel === 0) { const inp = this.list.el.querySelector("input"); inp.setSelectionRange(inp.value.length, inp.value.length); }
	}
	typed(v) {
		this.q = v;
		clearTimeout(this.t);
		this.t = setTimeout(async () => {
			if (!this.q.trim()) { this.games = []; this.sel = 0; this.draw(); return; }
			try {
				const j = await ss1({ cmd: "games", q: this.q.trim(), limit: 300 });
				if (topScr() !== this || v !== this.q) return;
				this.games = j.games;
				this.sel = 0;
				this.draw();
			} catch (e) { toast(e.message); }
		}, 250);
	}
}

/* A on a game: it starts on the TV, as in Console Mode */
async function play(g) {
	try { await ss1({ cmd: "launch", id: g.id }); toast(`Starting ${g.title} on the TV`); } catch (e) { toast(e.message); }
}

/* ---------- Settings: this PC and the SS1 ---------- */
class Settings {
	constructor() { this.crumb = "Settings"; this.sel = 0; }
	render(root) {
		const st = S.status || {};
		const items = [
			{ label: "SuperStation", t: S.paired ? `${S.address || ""}${S.connected ? "" : " (not answering)"}` : "Not paired", dim: !S.connected },
			{ label: "Console Mode", t: st.frontend_version ? `v${st.frontend_version}` : "?", dim: true },
			{ label: "Games", t: st.games !== undefined ? n(st.games) : "?", dim: true },
			{ t: "Look for the SuperStation again", open: async () => { try { const j = await api("/api/reconnect", { now: true }); toast(j.found ? (j.moved ? `Found it at ${j.address}` : "It's answering") : "Not found on this network"); await refreshState(); show(this); } catch (e) { toast(e.message); } } },
			{ t: "Look at the games again", open: async () => { try { await ss1({ cmd: "rescan" }); await refreshState(); toast("Done"); show(this); } catch (e) { toast(e.message); } } },
			{ t: "Pair with another SuperStation", open: () => push(Connect) },
			{ t: "Forget this SuperStation", open: async () => { await api("/api/forget", {}); S.paired = false; S.connected = false; replace(Connect); } },
			{ label: "This app", t: `${S.appName || ""} ${S.appVersion || ""}`, dim: true },
			{ t: "Exit", open: () => api("/api/quit", {}).then(() => window.close()) },
		];
		this.list = new List(root, items, { sel: this.sel, cls: "wide", onSelect: (it, i) => (this.sel = i) });
	}
}

/* ---------- start ---------- */
(async () => {
	await refreshState();
	replace(S.paired ? Home : Connect);
	setInterval(refreshState, 15000);
	/* the tray asked for the window: nothing to switch to in the base app, but keep the session alive */
	setInterval(() => api("/api/ping").catch(() => {}), 5000);
})();
