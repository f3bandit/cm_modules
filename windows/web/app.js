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
			: `<div class="row${i === this.sel ? " sel" : ""}${it.dim ? " dim" : ""}${it.head ? " head" : ""}" data-i="${i}">${it.label ? `<span class="lbl">${esc(it.label)}</span>` : ""}<span class="t">${esc(it.t)}</span>${it.v !== undefined && it.v !== "" ? `<span class="v">${esc(it.v)}</span>` : ""}</div>`).join("");
		this.el.querySelectorAll(".row").forEach(r => {
			const i = +r.dataset.i;
			r.addEventListener("mousedown", e => {
				if (e.button !== 0 || this.items[i].head) return;
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
	select(i, first, dir = 1) {
		if (!this.items.length) { this.page(); return; }
		i = Math.max(0, Math.min(this.items.length - 1, i));
		/* group headings can't be chosen: the next row in the direction of travel */
		for (let k = 0; this.items[i].head && k < this.items.length; k++) {
			i += dir;
			if (i >= this.items.length) { i = this.items.length - 1; dir = -1; }
			if (i < 0) { i = 0; dir = 1; }
		}
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
	page() {
		if (this.opts.count === false) return;
		const real = this.items.filter(x => !x.head), at = real.indexOf(this.items[this.sel]);
		$("#page").textContent = real.length ? `${at + 1} / ${real.length}` : "";
	}
	/* up and down go round, as on the TV; a page (left/right, LB/RB) stops at the ends */
	move(d, clamp) {
		const k = this.items.length;
		if (!k) return;
		let i = this.sel + d;
		if (clamp || Math.abs(d) > 1) i = Math.max(0, Math.min(k - 1, i));
		else i = (i + k) % k;
		this.select(i, false, d < 0 ? -1 : 1);
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

/* ---------- Home: the tools, Phosphor for Windows' made for Console Mode ----------
   Sections in groups (the SuperStation, tools on this PC, this PC), each with its tabs: the same tools as
   Phosphor for Windows, each one tailored to Console Mode as it comes over. */
const SECTIONS = [
	{ grp: "SuperStation" },
	{ name: "Box art & details", d: "Your games as Console Mode lists them: box art and details looked up on this PC or from a file, saved where Console Mode reads them.", tabs: [["Games", () => push(Groups)], ["Sources & logins"]] },
	{ name: "Power & updates", d: "Restart or shut down safely, the restore points made before MiSTer Main and Linux updates, file sharing, and backups of MiSTer and Linux on this PC.", tabs: [["Power"], ["Updates & restore points", () => push(Guard)], ["File sharing"], ["MiSTer & Linux backups"]] },
	{ name: "Update All", d: "Every Update All setting from the PC, and run it. Before it runs, a restore point of MiSTer's Main and Linux is made so they can be put back.", tabs: [["Update All", () => push(UpdateAll)]] },
	{ name: "Storage", d: "The SD card and the NVMe/USB drives: health, free space, TRIM, and the offline SD card repair.", tabs: [["SD card"], ["NVMe / USB"]] },
	{ name: "Saves & memory cards", d: "The memory card in front port 1 (back it up, check it, put a backup back, its saves), backups of your saves, cloud saves.", tabs: [["Memory cards"], ["Backups"], ["Cloud saves"]] },
	{ name: "Controllers", d: "The front-port controller tester, the controller mappings and their backups, RetroSpy.", tabs: [["Controllers"], ["Backups"], ["RetroSpy"]] },
	{ name: "Devices", d: "Bluetooth controllers, the front ports, what's plugged into USB.", tabs: [["Bluetooth"], ["Front ports"], ["USB"]] },
	{ name: "Network", d: "The SS1's network, and network shares for games and saves.", tabs: [["Network"]] },
	{ name: "Video", d: "MiSTer's video settings, tried with Keep: they go back by themselves if the picture is lost.", tabs: [["Video settings"]] },
	{ name: "Remote control", d: "Console Mode's buttons from this PC.", tabs: [["Remote control"]] },
	{ name: "Screenshots", d: "Take a screenshot of the TV, see them and save them on this PC.", tabs: [["Screenshots"]] },
	{ name: "Advanced", d: "MiSTer.ini, and MiSTer's scripts.", tabs: [["MiSTer.ini"], ["Scripts"]] },
	{ grp: "Tools" },
	{ name: "Diagnostics", d: "The SS1's checks in one place: drives, temperatures, the logs.", tabs: [["Dashboard"]] },
	{ name: "SD card flash & health", d: "On this PC: write a fresh SuperStation SD card, check a card and repair it.", tabs: [["Flash a card"], ["Card health"]] },
	{ grp: "This PC" },
	{ name: "Backups", d: "Everything this app backs up goes in one folder: on this PC (Documents › SS1 Console Mode › Backups, or a folder you pick) and on the SS1 (ConsoleMode/ss1/backups), one subfolder per kind.", tabs: [["Backups", () => push(Backups)]] },
	{ name: "Notifications", d: "Warnings from the SS1 (a drive filling up, an SD card not shut down safely), also as Windows notifications.", tabs: [["Notifications"]] },
	{ name: "Connection", d: "The SuperStation this PC is paired with: look for it again, pair with another one, forget it.", tabs: [["Connection", () => push(Settings)]] },
	{ name: "About", d: "This app's version, updating it, the open-source licences.", tabs: [["About", () => push(Settings)]] },
];
class Home {
	constructor() { this.crumb = "Tools"; this.sel = 0; this.hints = [["A", "OK"]]; }
	render(root) {
		const p = h(`<div class="note"></div>`).firstChild;
		root.appendChild(p);
		const items = [];
		for (const x of SECTIONS) {
			if (x.grp) { items.push({ t: x.grp, head: true }); continue; }
			const ready = x.tabs.filter(t => t[1]).length;
			items.push({
				t: x.name, dim: !ready,
				focus: () => (p.innerHTML = `<h2 class="ph">${esc(x.name)}</h2>${esc(x.d)}<div class="tablist">${x.tabs.map(t => `<div class="${t[1] ? "" : "soon"}">${esc(t[0])}${t[1] ? "" : " · coming"}</div>`).join("")}</div>`),
				open: () => (ready ? push(Section, x) : toast(`${x.name} isn't here yet`)),
			});
		}
		this.list = new List(root, items, { sel: this.sel || 1, onSelect: (it, i) => (this.sel = i) });
	}
	act(a) { return a === "b"; }    /* Home has no Back */
}
/* a section's tabs, as a list (a section with one ready tab opens it straight away) */
class Section {
	constructor(sec) { this.sec = sec; this.crumb = `Tools / ${sec.name}`; this.sel = 0; }
	render(root) {
		const ready = this.sec.tabs.filter(t => t[1]);
		if (ready.length === 1 && this.sec.tabs.length === 1) { stack.pop(); ready[0][1](); return; }
		const p = h(`<div class="note"></div>`).firstChild;
		root.appendChild(p);
		this.list = new List(root, this.sec.tabs.map(t => ({ t: t[0], dim: !t[1], focus: () => (p.innerHTML = `<h2 class="ph">${esc(t[0])}</h2>${t[1] ? "" : `<span class="soon">Coming from Phosphor for Windows</span>`}`), open: () => (t[1] ? t[1]() : toast(`${t[0]} isn't here yet`)) })), { sel: this.sel, onSelect: (it, i) => (this.sel = i) });
	}
}

/* ---------- Load Game: groups, systems, then a system's folders and games ---------- */
class Groups {
	constructor() { this.crumb = "Box art & details"; this.sel = 0; }
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
	constructor(group) { this.group = group; this.crumb = `Box art & details / ${group}`; this.sel = 0; }
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
		this.crumb = ["Box art & details", group, sys, ...(path ? path.split("/") : [])].join(" / ");
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
				: { t: e.title, g: e, focus: () => artInto(this.p, e), open: () => push(Game, e.id, this.crumb) });
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
			const items = j.games.map(g => ({ t: g.title, v: g.system, focus: () => artInto(this.p, g), open: () => push(Game, g.id, this.crumb) }));
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
			.concat(this.games.map(g => ({ t: g.title, v: g.system, focus: () => artInto(this.p, g), open: () => push(Game, g.id) })));
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

/* ---------- a game: its box art and details; play it, edit it, new box art ---------- */
const DETAILS = [["title", "Title"], ["year", "Year"], ["developer", "Developer"], ["publisher", "Publisher"], ["genre", "Genre"], ["players", "Players"], ["region", "Region"]];
class Game {
	constructor(id, from) { this.id = id; this.from = from; this.crumb = "Game"; this.sel = 0; }
	render(root) {
		this.root = root;
		if (this.g) this.draw(); else this.load();
	}
	async load() {
		try {
			this.g = (await ss1({ cmd: "game", id: this.id })).game;
			if (topScr() === this) this.draw();
		} catch (e) { toast(e.message); }
	}
	draw() {
		const g = this.g, root = this.root;
		root.innerHTML = "";
		this.crumb = `${this.from || g.system_name || g.system} / ${g.title}`;
		$("#crumb").textContent = this.crumb;
		const art = h(`<div class="game-art"></div>`).firstChild;
		root.appendChild(art);
		artInto(art, { id: g.id, art: !!g.art, title: g.title, artv: this.artv });
		const meta = [g.year, g.developer, g.genre, g.players ? `${g.players} player${g.players === "1" ? "" : "s"}` : ""].filter(Boolean).join(" · ");
		const side = h(`<div class="game-side"><h1>${esc(g.title)}</h1><div class="meta">${esc(meta || (g.system_name || g.system))}</div>
			<div class="desc">${esc(g.description || "No description yet.")}</div></div>`).firstChild;
		root.appendChild(side);
		const items = [
			{ t: "Play on TV", open: () => this.play() },
			{ t: "Edit details", open: () => push(EditGame, this) },
			{ t: "Box art from a file…", open: () => this.pickArt() },
			{ t: g.play_count ? `Played ${g.play_count} time${g.play_count === 1 ? "" : "s"}` : "Not played yet", dim: true },
		];
		this.list = new List(side, items, { sel: this.sel, visible: 6, count: false, onSelect: (it, i) => (this.sel = i) });
		this.file = h(`<input type="file" accept="image/*" hidden>`).firstChild;
		this.file.onchange = e => this.putArt(e.target.files[0]);
		root.appendChild(this.file);
	}
	async play() {
		try { await ss1({ cmd: "launch", id: this.id }); toast(`Starting ${this.g.title} on the TV`); } catch (e) { toast(e.message); }
	}
	pickArt() { this.file.value = ""; this.file.click(); }
	async putArt(f) {
		if (!f) return;
		toast("Sending the box art…");
		try {
			const r = await fetch(`/api/art/${this.id}`, { method: "PUT", headers: { "X-App-Key": KEY }, body: f });
			const j = await r.json();
			if (!j.ok) throw new Error(j.error || "the SS1 didn't take it");
			this.g = j.game || this.g;
			this.artv = Date.now();
			toast("Box art saved on the SS1");
			if (topScr() === this) this.draw();
		} catch (e) { toast(e.message); }
	}
}
class EditGame {
	constructor(gs) { this.gs = gs; this.g = gs.g; this.crumb = `${gs.g.title} / Edit details`; this.desc = gs.g.description || ""; this.sel = 0; this.hints = [["B", "Back"], ["A", "OK"]]; }
	render(root) {
		const items = DETAILS.map(([k, l]) => ({ label: l, key: k, input: { value: this.vals ? this.vals[k] : (this.g[k] || "") }, open: () => this.list.move(1) }));
		items.push({ label: "Description", t: this.desc ? this.desc.replace(/\s+/g, " ").slice(0, 60) + (this.desc.length > 60 ? "…" : "") : "(none)", open: () => { this.keep(); push(EditText, this); } });
		items.push({ t: "Save", open: () => this.save() });
		this.list = new List(root, items, { sel: this.sel, cls: "wide", onSelect: (it, i) => (this.sel = i) });
		root.append(h(`<div class="note" style="top:440px;left:97px;width:1086px">Saved in the system's gamelist.xml on the SS1. Console Mode shows them after <b>Settings › Game Library › Import gamelist.xml</b>.</div>`));
	}
	keep() { this.vals = {}; this.list.items.forEach(it => { if (it.key) this.vals[it.key] = it.input.value; }); }
	async save() {
		this.keep();
		const set = {};
		for (const [k] of DETAILS) { const v = this.vals[k].trim(); if (v !== (this.g[k] || "")) set[k] = v; }
		if ((this.desc || "") !== (this.g.description || "")) set.description = this.desc;
		if (set.title === "") { toast("The title can't be empty"); return; }
		if (!Object.keys(set).length) { toast("Nothing changed"); return; }
		try {
			const j = await ss1({ cmd: "edit", id: this.g.id, set });
			this.gs.g = j.game;
			toast("Saved on the SS1");
			back();
		} catch (e) { toast(e.message); }
	}
}
class EditText {
	constructor(ed) { this.ed = ed; this.crumb = `${ed.g.title} / Description`; this.hints = [["B", "Done"]]; }
	render(root) {
		const t = h(`<textarea class="desc-edit" spellcheck="true"></textarea>`).firstChild;
		t.value = this.ed.desc;
		t.addEventListener("input", () => (this.ed.desc = t.value));
		root.appendChild(t);
		setTimeout(() => t.focus(), 0);
	}
	act(a) { if (a === "b") { back(); return true; } return true; }
}

/* ---------- a question, Console Mode style: the text, then the answers as a list ---------- */
function ask(title, text, options, def = 0) {
	return new Promise(res => push(Dialog, title, text, options, def, res));
}
class Dialog {
	constructor(title, text, options, def, res) { this.title = title; this.text = text; this.options = options; this.def = def; this.res = res; this.crumb = title; this.hints = [["B", "Back"], ["A", "OK"]]; }
	render(root) {
		root.append(h(`<div class="dlg"><h2 class="ph">${esc(this.title)}</h2><div class="dlg-t">${esc(this.text)}</div></div>`));
		this.list = new List(root, this.options.map((o, i) => ({ t: o, open: () => this.done(i) })), { sel: this.def, cls: "dlg-list", count: false });
	}
	done(i) { stack.pop(); show(topScr()); if (topScr().restore) topScr().restore(); this.res(i); }
	act(a) { if (a === "b") { this.done(null); return true; } return false; }
}

/* ---------- a job on the SS1 (Update All, a rollback...): its output as it runs ---------- */
class Job {
	constructor(title, after) { this.title = title; this.after = after; this.crumb = title; this.hints = [["B", "Back"]]; }
	render(root) {
		this.out = h(`<pre class="job"><span class="spin"></span>Starting…</pre>`).firstChild;
		root.appendChild(this.out);
		this.poll();
	}
	async poll() {
		if (topScr() !== this) return;
		try {
			const j = (await ss1({ cmd: "job" })).job;
			const at = this.out.scrollTop + this.out.clientHeight >= this.out.scrollHeight - 30;
			this.out.textContent = j.output || "";
			if (j.busy) this.out.insertAdjacentHTML("beforeend", `\n<span class="spin"></span>`);
			else this.out.insertAdjacentHTML("beforeend", `\n\n<b>${j.ok ? "Done." : `It stopped (exit ${j.exit}).`}</b>`);
			if (at) this.out.scrollTop = this.out.scrollHeight;
			$("#page").textContent = j.busy ? "running" : j.ok ? "done" : "stopped";
			if (!j.busy) { if (this.after) this.after(j); return; }
		} catch (e) { /* the SS1 busy or restarting: asked again */ }
		setTimeout(() => this.poll(), 1500);
	}
	act(a) {
		if (a === "up") { this.out.scrollTop -= 120; return true; }
		if (a === "down") { this.out.scrollTop += 120; return true; }
		if (a === "lb" || a === "left") { this.out.scrollTop -= 480; return true; }
		if (a === "rb" || a === "right") { this.out.scrollTop += 480; return true; }
		return false;
	}
}

/* ---------- Update All: its whole settings list, as Update All itself reads it on the SS1 ----------
   ss1remote runs Update All's own settings code (ua_bridge.py, from Phosphor): the menus as Update All shows
   them, changes saved the way its menu saves them, uninstalls through its own database manager. Nothing is
   blocked; MiSTer's Main and Linux are protected by a restore point made before every run from here. */
class UpdateAll {
	constructor() { this.crumb = "Tools / Update All"; this.stack = []; this.sel = {}; this.L = null; }
	get hints() {
		const e = this.cur;
		const h2 = [["Y", "Run Update All"]];
		if (e && this.installed(e) && e.uninstall) h2.push(["X", "Uninstall"]);
		h2.push(["B", "Back"], ["A", e && e.submenu && !e.toggles ? "Open" : "Change"]);
		return h2;
	}
	render(root) {
		this.root = root;
		this.p = h(`<div class="note"></div>`).firstChild;
		root.appendChild(this.p);
		if (this.L) this.fill(); else { this.p.innerHTML = `<span class="spin"></span>Reading Update All's settings on the SS1… (several seconds the first time)`; this.load(); }
	}
	restore() { this.load(); }
	async load() {
		try {
			const r = await ss1({ cmd: "ua" });
			if (topScr() !== this) return;
			if (!r.menus) {
				if (r.load_error) { this.p.innerHTML = `Update All's settings didn't load: run Update All once from MiSTer's Scripts, then try again.<br><br><span class="soon">${esc(r.load_error)}</span>`; return; }
				setTimeout(() => this.load(), 1500);
				return;
			}
			this.L = r;
			if (!this.stack.every(m => r.menus[m])) this.stack = [];
			this.fill();
			if (r.busy || r.loading) setTimeout(() => { if (topScr() === this) this.load(); }, 2000);
		} catch (e) { if (topScr() === this) this.p.innerHTML = esc(e.message); }
	}
	menu(id) { return (this.L.menus || {})[id] || { title: "", entries: [] }; }
	here() { return this.stack.length ? this.stack[this.stack.length - 1] : this.L.root; }
	installed(e) {
		const id = e.uninstall && e.uninstall.db_ids && e.uninstall.db_ids[0];
		return !!id && (this.L.installed || []).some(x => x.toLowerCase() === id.toLowerCase());
	}
	acts(e) { return e.toggles || (!e.submenu && (e.actions || []).includes("ok")); }
	val(e) {
		if (e.submenu && !e.toggles) return "›";
		if (e.value === "true" || e.value === "false") return e.value === "true" ? "on" : "off";
		if (e.value) return e.value;
		return this.acts(e) ? (e.on ? "on" : "off") : "";
	}
	fill() {
		const old = this.root.querySelector(".list");
		if (old) old.remove();
		const m = this.menu(this.here()), key = this.here();
		this.crumb = ["Tools", "Update All", ...this.stack.map(id => this.menu(id).title || id)].join(" / ");
		$("#crumb").textContent = this.crumb;
		const items = m.entries.map(e => ({ t: e.title, v: this.val(e), e, focus: () => this.detail(e), open: () => (e.submenu && !e.toggles ? this.open(e) : this.acts(e) ? this.change(e) : null) }));
		this.list = new List(this.root, items, { sel: this.sel[key] || 0, onSelect: (it, i) => (this.sel[key] = i), empty: "Nothing in this menu" });
		if (!items.length) this.detail(null);
	}
	detail(e) {
		this.cur = e;
		setHints(this.hints);
		if (!e) { this.p.innerHTML = `<h2 class="ph">Update All ${esc(this.L.version || "")}</h2>Update All's own settings, read from the SS1.`; return; }
		const st = e.status || {}, said = [e.desc, ...(e.info || [])].filter(Boolean).join("\n");
		this.p.innerHTML = `<h2 class="ph">${esc(e.title)}</h2>${said ? `<div class="said">${esc(said)}</div>` : ""}
			${st.status === "protected" ? `<div class="prot">Protected</div><div class="said">${esc(st.note || "")}</div>` : ""}
			${this.installed(e) ? `<div class="soon" style="margin-top:12px">Installed</div>` : ""}
			${this.L.busy ? `<div class="soon" style="margin-top:12px">Uninstalling ${esc(this.L.busy_with || "")}…</div>` : ""}`;
	}
	open(e) { if (this.stack.length < 8) { this.stack.push(e.submenu); this.fill(); } }
	act(a) {
		if (a === "b" && this.stack.length) { this.stack.pop(); this.fill(); return true; }
		if (a === "y") { this.run(); return true; }
		if (a === "x") { if (this.cur && this.cur.uninstall && this.installed(this.cur)) this.uninstall(this.cur); return true; }
		return false;
	}
	async change(e, choice) {
		if (this.L.busy) { toast("Busy uninstalling; try again when it's done"); return; }
		const args = [e.menu, e.index, (e.actions || []).includes("toggle") ? "toggle" : "ok"];
		if (choice !== undefined) args.push(choice);
		let r;
		try { r = await api("/api/call", { cmd: "ua", do: "act", args }); } catch (x) { toast(x.message); return; }
		if (r.wait) { toast(r.error); return; }
		if (r.prompt) {
			const p = r.prompt;
			if (p.ui === "confirm") {
				const pick = await ask(p.header || "Update All", (p.text || []).join("\n"), p.buttons || ["OK"]);
				if (pick !== null) return this.change(e, pick);
			} else await ask(p.header || "Update All", (p.text || []).join("\n"), ["OK"]);
		}
		if (r.navigate && this.L.menus[r.navigate]) { this.stack.push(r.navigate); }
		else if (!r.ok) toast(r.error || "That didn't work");
		await this.load();
	}
	async uninstall(e) {
		let p;
		try { p = await ss1({ cmd: "ua", do: "preview", args: e.uninstall.db_ids }); } catch (x) { toast(x.message); return; }
		const files = Number(p.files || 0), bytes = Number(p.bytes || 0);
		const size = bytes >= 1e9 ? (bytes / 1e9).toFixed(1) + " GB" : bytes >= 1e6 ? Math.round(bytes / 1e6) + " MB" : Math.round(bytes / 1e3) + " KB";
		const pick = await ask(`Remove ${e.title}?`, `Only the files Update All installed for it are removed, from its own list: ${n(files)} files, ${size}. It's also turned off in Update All, so the next run doesn't put it back.`, ["Keep it", `Remove ${n(files)} files`]);
		if (pick !== 1) return;
		try { await ss1({ cmd: "ua", do: "uninstall", args: e.uninstall.db_ids, title: e.title }); toast("Uninstalling: Update All's Downloader removes the files"); } catch (x) { toast(x.message); return; }
		this.load();
	}
	async run() {
		const pick = await ask("Run Update All?", "Update All runs on the SuperStation now. First a restore point of MiSTer's Main and Linux is saved, so they can be put back from Updates & restore points. Console Mode keeps running; if Update All asks for a restart at the end, restart from Power.", ["Not now", "Run Update All"], 1);
		if (pick !== 1) return;
		try { await ss1({ cmd: "ua", do: "run" }); } catch (x) { toast(x.message); return; }
		this.L = null;
		push(Job, "Tools / Update All / Running");
	}
}

/* ---------- Updates & restore points: MiSTer's Main and Linux, as they were before each update ---------- */
class Guard {
	constructor() { this.crumb = "Tools / Power & updates / Updates & restore points"; this.sel = 0; }
	render(root) {
		this.root = root;
		this.p = h(`<div class="note"><span class="spin"></span></div>`).firstChild;
		root.appendChild(this.p);
		this.load();
	}
	restore() { this.load(); }
	async load() {
		let g;
		try { g = await ss1({ cmd: "guard" }); } catch (e) { this.p.textContent = e.message; return; }
		if (topScr() !== this) return;
		const old = this.root.querySelector(".list");
		if (old) old.remove();
		if (!g.installed) { this.p.textContent = "update-guard isn't installed with the app link on the SS1."; return; }
		const intro = () => (this.p.innerHTML = `<h2 class="ph">Updates & restore points</h2>Before Update All runs from this app, a restore point of MiSTer's Main, the menu core, the kernel and Linux is saved. Rolling back puts them as they were; your games, saves and Console Mode aren't touched.${g.job && g.job.busy ? `<br><br><span class="spin"></span>Working: ${esc(g.job.name)}` : ""}`);
		const reset = (g.files || []).filter(f => f.reset);
		const items = [
			{ label: "Linux", t: g.linux || "?", dim: true, focus: intro },
			{ label: "Last update", t: g.updated_at ? `${g.updated_at}${g.updated_from ? ` (from ${g.updated_from})` : ""}` : "none seen yet", dim: true, focus: intro },
		];
		if (g.awaiting && reset.length) {
			const txt = `The last Linux update put these back to stock:\n${reset.map(f => f.path).join("\n")}`;
			items.push({ t: "Put my settings back", focus: () => (this.p.innerHTML = `<h2 class="ph">Put my settings back</h2><div class="said">${esc(txt)}</div>`), open: () => this.job("restore", { items: reset.map(f => f.path) }, "Putting your settings back") });
			items.push({ t: "Keep the new ones", focus: () => (this.p.innerHTML = `<h2 class="ph">Keep the new ones</h2><div class="said">${esc(txt)}</div>`), open: () => this.job("keep", {}, "Keeping the new settings") });
		}
		items.push({ t: "Save a restore point now", focus: () => (this.p.innerHTML = `<h2 class="ph">Save a restore point now</h2>MiSTer's Main, the menu core, the kernel and Linux as they are now. Linux takes a minute (about 400 MB).`), open: () => this.job("save_point", {}, "Saving a restore point") });
		const pts = g.points || [];
		if (!pts.length) items.push({ t: "No restore points yet", dim: true, focus: intro });
		for (const p of pts) items.push({
			t: p.label || p.name, v: (p.created || "") + (p.complete ? "" : " · partial"),
			focus: () => (this.p.innerHTML = `<h2 class="ph">${esc(p.label || p.name)}</h2><div class="kv"><span>Saved</span><span>${esc(p.created || "?")}</span></div><div class="kv"><span>Linux</span><span>${esc(p.linux || "?")}</span></div><br>Rolling back puts MiSTer's Main, the menu core, the kernel and Linux as they were then. Then restart.`),
			open: async () => {
				const pick = await ask(p.label || p.name, `Saved ${p.created || "?"}, Linux ${p.linux || "?"}.`, ["Back", "Roll back to this", "Delete this restore point"]);
				if (pick === 1) this.job("rollback", { items: [p.name] }, "Rolling back");
				if (pick === 2) this.job("delete", { items: [p.name] }, "Deleting the restore point");
			},
		});
		this.list = new List(this.root, items, { sel: this.sel, onSelect: (it, i) => (this.sel = i) });
	}
	async job(d, extra, title) {
		try { await ss1(Object.assign({ cmd: "guard", do: d }, extra)); } catch (e) { toast(e.message); return; }
		push(Job, `Tools / Updates & restore points / ${title}`);
	}
}

/* ---------- Backups: the one folder on the PC and the one on the SS1 ---------- */
const size = b => b >= 1e9 ? (b / 1e9).toFixed(1) + " GB" : b >= 1e6 ? Math.round(b / 1e6) + " MB" : b > 0 ? Math.max(1, Math.round(b / 1e3)) + " KB" : "empty";
class Backups {
	constructor() { this.crumb = "Tools / Backups"; this.sel = 0; }
	render(root) {
		this.root = root;
		this.p = h(`<div class="note"><span class="spin"></span></div>`).firstChild;
		root.appendChild(this.p);
		this.load();
	}
	async load(open) {
		let j;
		try { j = await api("/api/backups", open ? { open } : undefined); } catch (e) { this.p.textContent = e.message; return; }
		if (topScr() !== this) return;
		const old = this.root.querySelector(".list");
		if (old) old.remove();
		const pc = j.pc, ss = j.ss1;
		const items = [{ t: "On this PC", head: true }];
		items.push({ t: "Open the folder", v: size(pc.bytes), focus: () => (this.p.innerHTML = `<h2 class="ph">On this PC</h2><div class="said">${esc(pc.folder)}</div><br>Every backup this app makes on the PC goes here.`), open: () => this.load("pc") });
		for (const k of pc.kinds) items.push({ t: k.name, v: k.exists ? size(k.bytes) : "none yet", dim: !k.exists, focus: () => (this.p.innerHTML = `<h2 class="ph">${esc(k.name)}</h2><div class="said">${esc(k.path)}</div>`), open: () => this.load(k.id) });
		items.push({ label: "Move it to", input: { value: "", placeholder: "a folder, e.g. D:\\SS1 Backups" }, open: async v => { try { const r = await api("/api/backups", { folder: (v || "").trim() }); if (!r.ok) toast(r.error); else { toast("New backups go there now"); this.load(); } } catch (e) { toast(e.message); } } });
		items.push({ t: "On the SuperStation", head: true });
		if (!ss || !ss.ok) items.push({ t: "Not connected", dim: true, focus: () => (this.p.innerHTML = `<h2 class="ph">On the SuperStation</h2>Its backups show here when the app is connected.`) });
		else {
			items.push({ t: "ConsoleMode/ss1/backups", v: size(ss.bytes), dim: true, focus: () => (this.p.innerHTML = `<h2 class="ph">On the SuperStation</h2><div class="said">${esc(ss.folder)}</div><br>Every backup made on the SS1 goes here: restore points before updates, settings inside Linux, memory cards.`) });
			for (const k of ss.kinds) items.push({ t: k.name, v: k.exists ? size(k.bytes) : "none yet", dim: true, focus: () => (this.p.innerHTML = `<h2 class="ph">${esc(k.name)}</h2><div class="said">${esc(k.path)}</div>`) });
		}
		this.list = new List(this.root, items, { sel: this.sel || 1, onSelect: (it, i) => (this.sel = i) });
	}
}

/* ---------- Settings: this PC and the SS1 ---------- */
class Settings {
	constructor() { this.crumb = "Tools / Connection & about"; this.sel = 0; }
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
