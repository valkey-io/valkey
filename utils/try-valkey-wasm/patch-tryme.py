#!/usr/bin/env python3
"""Turn the sign-on prototype into the real try-valkey page. Every replacement must match exactly once."""
import sys

p = sys.argv[1]
s = open(p).read()
n_before = len(s)

def rep(old, new, count=1):
    global s
    c = s.count(old)
    if c != count:
        sys.exit(f"anchor matched {c}x (expected {count}):\n{old[:120]}")
    s = s.replace(old, new)

# ---- brief -----------------------------------------------------------------
rep("""<!-- BRIEF: Valkey sign-on -> TRY-VALKEY handoff prototype (v2, three-phase). Derived from valkey-signon.html.""",
    """<!-- try-valkey: the Valkey sign-on -> a REAL valkey-server (compiled to WebAssembly) running in this tab.
     v3: the mock loader and mock CLI of the v2 prototype are replaced by the actual module. The download that
     drives the tape loader is valkey-server.wasm; the boot readout's milestones are real events (FETCH = bytes
     landed, COMPILE = WebAssembly instantiated, SERVER = "Server initialized" in the server log, VALKEY READY =
     "Ready to accept connections"); the terminal is a real client on an in-process connection, with
     valkey-cli's own line splitting (sdssplitargs, in the module) and a port of its TTY formatter.
     ?mock=1 keeps the v2 stand-ins (also the automatic fallback if the .wasm cannot be fetched).
     ?dl=SECONDS / ?boot=SECONDS now mean MINIMUM durations (the fetch is paced, the boot delayed) so the vamps
     can be demonstrated on a fast connection; 0/0 = as fast as the machine allows.

     Original brief (v2) follows.
     BRIEF: Valkey sign-on -> TRY-VALKEY handoff prototype (v2, three-phase). Derived from valkey-signon.html.""")

# ---- boot readout labels ----------------------------------------------------
rep("""    <text class="bl anim" x="64" y="48">&gt; KERNEL ............ <tspan class="st" fill="#eaf0ff">OK</tspan></text>
    <text class="bl anim" x="64" y="70">&gt; ROOTFS ............ <tspan class="st" fill="#eaf0ff">OK</tspan></text>
    <text class="bl anim" x="64" y="92">&gt; NETWORK ........... <tspan class="st" fill="#eaf0ff">ONLINE</tspan></text>
    <text class="bl anim" x="64" y="114">&gt; VALKEY ............ <tspan class="st" fill="#eaf0ff">READY</tspan></text>""",
    """    <text class="bl anim" x="64" y="48">&gt; FETCH ............. <tspan class="st" fill="#eaf0ff">OK</tspan></text>
    <text class="bl anim" x="64" y="70">&gt; COMPILE ........... <tspan class="st" fill="#eaf0ff">OK</tspan></text>
    <text class="bl anim" x="64" y="92">&gt; SERVER ............ <tspan class="st" fill="#eaf0ff">UP</tspan></text>
    <text class="bl anim" x="64" y="114">&gt; VALKEY ............ <tspan class="st" fill="#eaf0ff">READY</tspan></text>""")

# ---- start panel ------------------------------------------------------------
rep("""VALKEY · SIGN-ON → TRY-VALKEY (mock)""", """VALKEY · SIGN-ON → TRY-VALKEY <span id="engine-tag">(wasm)</span>""")
rep("""      <span style="font-size:11px;opacity:.6">LOADER</span>
      <label><input type="radio" name="preset" value="1,1"><span>FAST 1s/1s</span></label>
      <label><input type="radio" name="preset" value="4,3" checked><span>TYPICAL 4s/3s</span></label>
      <label><input type="radio" name="preset" value="20,6"><span>SLOW 20s/6s</span></label>
      <label style="border-color:transparent"><span style="font-size:11px;opacity:.7">DL <input""",
    """      <span style="font-size:11px;opacity:.6">PACING (min)</span>
      <label><input type="radio" name="preset" value="0,0"><span>REAL 0s/0s</span></label>
      <label><input type="radio" name="preset" value="4,3" checked><span>TYPICAL 4s/3s</span></label>
      <label><input type="radio" name="preset" value="20,6"><span>SLOW 20s/6s</span></label>
      <label style="border-color:transparent"><span style="font-size:11px;opacity:.7">DL <input""")
rep("""<div id="term" class="mono" role="region" aria-label="Valkey CLI (mock)">""",
    """<div id="term" class="mono" role="region" aria-label="Valkey CLI">""")
rep("""<span class="p" id="tprompt">127.0.0.1:6379&gt; </span>""", """<span class="p" id="tprompt">try-valkey&gt; </span>""")

# ---- loader text ------------------------------------------------------------
rep("""    return `> LOADING EMULATOR ... <b>${String(Math.floor(loader.progress * 100)).padStart(3)}%</b>  ${'#'.repeat(n)}${'.'.repeat(22 - n)}  ${mb} / 50.0 MB${stalled""",
    """    return `> LOADING ${REAL ? 'VALKEY-SERVER.WASM' : 'EMULATOR'} ... <b>${String(Math.floor(loader.progress * 100)).padStart(3)}%</b>  ${'#'.repeat(n)}${'.'.repeat(22 - n)}  ${mb} / ${(loader.total / 1048576).toFixed(1)} MB${stalled""")

# ---- real loader: inserted right before the MOCK CLI section ----------------
rep("""/* ============================ MOCK CLI ============================ */""",
    r"""/* ============================ REAL LOADER (wasm) ============================ */
// The same `loader` contract the machine polls, fed by real events:
//   download  = fetch of valkey-server.wasm, streamed so bytes/progress/stalls are genuine
//   boot      = WebAssembly instantiation, then the server's own log lines (print callback)
//   marks[]   = FETCH complete / COMPILE done / "Server initialized" / "Ready to accept connections"
const REAL = new URLSearchParams(location.search).get('mock') === null;
const REAL_MARKERS = [null, null, 'Server initialized', 'Ready to accept connections'];
let wasmModule = null, tv = null, engineMod = null;
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function startRealLoader(minDl, minBoot) {
  loader.phase = 'download'; loader.progress = 0; loader.bytes = 0; loader.lines = []; loader.ready = false; loader.marks = [false, false, false, false]; loader.lastBytesAt = performance.now();
  const t0 = performance.now();
  let bytes;
  try {
    const res = await fetch('valkey-server.wasm');
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    loader.total = Number(res.headers.get('content-length')) || 2.3 * 1048576;
    const reader = res.body.getReader(), chunks = [];
    for (;;) {
      const {done, value} = await reader.read();
      if (done) break;
      chunks.push(value); loader.bytes += value.length; loader.lastBytesAt = performance.now();
      loader.progress = Math.min(1, loader.bytes / loader.total);
      // pacing: hold the reader so the download takes at least minDl seconds (demo of the vamp on a fast link);
      // slice the wait so the stall detector (350 ms without bytes) is left for real stalls
      const shouldBeAt = t0 + minDl * 1000 * loader.progress;
      while (performance.now() < shouldBeAt) { await sleep(Math.min(120, shouldBeAt - performance.now())); loader.lastBytesAt = performance.now(); }
    }
    bytes = new Uint8Array(loader.bytes); let off = 0; for (const c of chunks) { bytes.set(c, off); off += c.length; }
    loader.total = loader.bytes; loader.progress = 1;
  } catch (e) {
    console.warn('try-valkey: wasm fetch failed, falling back to the mock', e);
    document.getElementById('engine-tag').textContent = '(mock: wasm unavailable)';
    return startLoader(Math.max(1, minDl), Math.max(1, minBoot));
  }
  loader.marks[0] = true; loader.lines.push(`wasm: fetched ${bytes.length.toLocaleString()} bytes in ${Math.round(performance.now() - t0)} ms`);
  loader.phase = 'boot';
  const tb = performance.now();
  const [{default: createValkey}, engine] = await Promise.all([import('./valkey-server.mjs'), import('./tryvalkey.mjs')]);
  engineMod = engine;
  const onLine = l => {
    if (/^\d+:M /.test(l)) loader.lines.push(l);                             // log lines only; the ASCII banner is not for the corner
    REAL_MARKERS.forEach((m, k) => { if (m && l.includes(m)) loader.marks[k] = true; });
    if (loader.marks[3] && !loader.ready) { loader.phase = 'ready'; loader.ready = true; }
  };
  wasmModule = await createValkey({wasmBinary: bytes.buffer, print: onLine, printErr: onLine, noInitialRun: true});
  loader.marks[1] = true; loader.lines.push(`wasm: instantiated in ${Math.round(performance.now() - tb)} ms`);
  const bootAt = tb + minBoot * 1000; if (performance.now() < bootAt) await sleep(bootAt - performance.now());
  engine.runMain(wasmModule, [...engine.DEFAULT_ARGS, '--loglevel', 'notice']);   // log lines stream into onLine synchronously here
  tv = engine.attachTryValkey(wasmModule, {hostLabel: 'try-valkey'});
}

/* ============================ MOCK CLI ============================ */""")

# ---- start: choose the loader -----------------------------------------------
rep("""  startLoader(...loaderParams());
  phase = 'load'; loadStart = nowS() + .12;""",
    """  if (REAL) startRealLoader(...loaderParams()); else startLoader(...loaderParams());
  phase = 'load'; loadStart = nowS() + .12;""")

# ---- terminal: real exec ------------------------------------------------------
rep("""function run(line) {
  tprint(`<span class="p">127.0.0.1:6379&gt; </span>${esc(line)}`);
  const a = tokenize(line); if (!a.length) return;""",
    """const tprompt = document.getElementById('tprompt');
function promptText() { return tv ? tv.session.prompt() : '127.0.0.1:6379> '; }
function printReply(text) {                                                  // valkey-cli TTY text -> coloured lines
  for (const l of text.replace(/\\n$/, '').split('\\n')) {
    const cls = l.startsWith('(error)') ? 'e' : (l.startsWith('(nil)') || l.startsWith('(empty')) ? 'd' : 'o';
    tprint(`<span class="${cls}">${esc(l)}</span>`);
  }
}
let termBusy = false;
async function run(line) {
  tprint(`<span class="p">${esc(promptText())}</span>${esc(line)}`);
  if (tv) {                                                                  // the real server
    const c = line.trim().toLowerCase();
    if (!c) return;
    if (c === 'clear') { tout.innerHTML = ''; return; }
    if (c === 'quit' || c === 'exit') { tprint('<span class="d">(this is a browser tab -- just close it)</span>'); return; }
    if (c === 'help' || c === '?') { tprint('<span class="d">A real valkey-server is running in this tab. Try: SET k v · GET k · INCR n · HSET h f v · HGETALL h · LPUSH l a b · LRANGE l 0 -1 · ZADD z 1 a · HELLO 3 · COMMAND DOCS GET</span>'); return; }
    termBusy = true;
    try { printReply(await tv.exec(line)); } finally { termBusy = false; tprompt.textContent = promptText(); }
    return;
  }
  const a = tokenize(line); if (!a.length) return;""")
rep("""  if (e.key === 'Enter') { const v = tin.value; tin.value = ''; tbuf.textContent = ''; if (v.trim()) { hist.push(v); hi = hist.length; } run(v); }""",
    """  if (e.key === 'Enter') { if (termBusy) return; const v = tin.value; tin.value = ''; tbuf.textContent = ''; if (v.trim()) { hist.push(v); hi = hist.length; } run(v); }""")

# ---- handoff: real log lines, pub/sub pushes ---------------------------------
rep("""    document.body.classList.add('termon'); bootWall = performance.now();
    tprint('<span class="d">1:M * Server initialized</span>'); tprint('<span class="d">1:M * Ready to accept connections tcp</span>'); tprint('&nbsp;'); tin.focus();""",
    """    document.body.classList.add('termon'); bootWall = performance.now();
    if (tv) {
      for (const l of loader.lines.filter(l => /Server initialized|Ready to accept connections/.test(l))) tprint(`<span class="d">${esc(l.replace(/^\\d+:M \\d+ \\w+ \\d+ [\\d:.]+ /, '1:M '))}</span>`);
      tprompt.textContent = promptText();
      const pushes = outs => { if (outs.length) { outs.forEach(printReply); } };
      tv.exec.onUnsolicited = pushes;
      setInterval(() => pushes(tv.tick()), 50);                              // serverCron, expiry, blocking timeouts, pub/sub
    } else { tprint('<span class="d">1:M * Server initialized</span>'); tprint('<span class="d">1:M * Ready to accept connections tcp</span>'); }
    tprint('&nbsp;'); tin.focus();""")

open(p, "w").write(s)
print(f"patched: {n_before} -> {len(s)} bytes")
