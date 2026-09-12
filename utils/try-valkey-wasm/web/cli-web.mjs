// cli-web.mjs -- xterm.js front end for try-valkey.
// Line editing (cursor keys, history, Ctrl-C/U/L, paste) is done here; command
// splitting happens in the wasm module (sdssplitargs) and reply formatting in
// cli-core.mjs, so what you see is what valkey-cli would print.
import { startTryValkey } from './tryvalkey.mjs';
import { buildHelpEntries, hintFor, completionsFor } from './cli-hints.mjs';

const HOST_LABEL = 'try-valkey';
const TICK_MS = 50; // drives serverCron/expiry/blocking timeouts + pub/sub pushes

export async function mountTryValkey({ container, createModule, Terminal, FitAddon }) {
  const term = new Terminal({
    cursorBlink: true, convertEol: true, fontSize: 14, scrollback: 5000,
    fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace',
    theme: { background: '#0b0e14', foreground: '#e6e6e6', cursor: '#7dd3fc' },
  });
  const fit = FitAddon ? new FitAddon() : null;
  if (fit) term.loadAddon(fit);
  term.open(container);
  fit?.fit();
  window.addEventListener('resize', () => fit?.fit());

  term.writeln('\x1b[2mStarting valkey-server (WebAssembly)...\x1b[0m');
  const t0 = performance.now();
  const tv = await startTryValkey(createModule, { hostLabel: HOST_LABEL, log: () => {} });
  const info = await tv.exec('info server');
  const version = /valkey_version:([^\\\r\n"]+)/.exec(info)?.[1] ?? '?';
  const os = /os:([^\\\r\n"]+)/.exec(info)?.[1] ?? '?';
  term.writeln(`\x1b[2mvalkey-server ${version} ready in ${Math.round(performance.now() - t0)} ms (${os.trim()}). ` +
    `Everything runs in this tab; nothing leaves your browser.\x1b[0m`);
  term.writeln('\x1b[2mType a command (e.g. SET greeting "hello", GET greeting). Ctrl-L clears, Up/Down for history.\x1b[0m');

  // ---- line editor state ----
  let line = '', cursor = 0, history = [], histIdx = -1, savedLine = '', busy = false;
  const promptStr = () => tv.session.prompt();

  // valkey-cli's linenoise hints/completion, fed by the real server's COMMAND DOCS
  let helpEntries = [];
  tv.execRaw('command docs').then((docs) => { if (docs && docs.elements) helpEntries = buildHelpEntries(docs); }).catch(() => {});
  const currentHint = () => {
    if (!helpEntries.length || cursor !== line.length) return '';
    const hint = hintFor(helpEntries, line, tv.splitArgs);
    // linenoise clips the hint so the row never wraps (a wrapped row cannot be redrawn with \r + clear-line)
    const room = term.cols - promptStr().length - line.length - 1;
    return room > 0 ? hint.slice(0, room) : '';
  };
  let tabState = null; // {original, list, idx} while cycling through completions

  function redraw() {
    // \r, clear line, prompt + line, grey hint, then move cursor back over hint+tail
    const hint = currentHint();
    term.write('\r\x1b[2K' + promptStr() + line + (hint ? `\x1b[90m${hint}\x1b[0m` : ''));
    const back = line.length - cursor + hint.length;
    if (back > 0) term.write(`\x1b[${back}D`);
  }
  function showPrompt() { term.write('\r\n' + promptStr()); }

  function tab() {
    if (!helpEntries.length) return;
    if (!tabState) {
      const list = completionsFor(helpEntries, line);
      if (!list.length) return;
      tabState = { original: line, list, idx: 0 };
    } else {
      tabState.idx = (tabState.idx + 1) % (tabState.list.length + 1); // ... then back to what was typed, like linenoise
    }
    line = tabState.idx < tabState.list.length ? tabState.list[tabState.idx] : tabState.original;
    cursor = line.length;
    redraw();
  }

  async function submit() {
    const l = line;
    tabState = null;
    term.write('\r\x1b[2K' + promptStr() + l + '\r\n'); // re-echo without the hint
    line = ''; cursor = 0; histIdx = -1;
    if (l.trim()) {
      if (history[history.length - 1] !== l) history.push(l);
      const cmd = l.trim().toLowerCase();
      if (cmd === 'clear') { term.clear(); term.write(promptStr()); return; }
      if (cmd === 'quit' || cmd === 'exit') { term.writeln('(this is a browser tab -- just close it)'); term.write(promptStr()); return; }
      if (cmd === 'help' || cmd === '?') {
        term.writeln('Commands are sent to a real valkey-server running in this tab.');
        term.writeln('Try: SET k v | GET k | INCR n | HSET h f v | HGETALL h | LPUSH l a b | LRANGE l 0 -1 | ZADD z 1 a | HELLO 3 | COMMAND DOCS GET');
        term.write(promptStr()); return;
      }
      busy = true;
      const out = await tv.exec(l);
      busy = false;
      term.write(out.replace(/\n/g, '\r\n'));
    }
    term.write(promptStr());
  }

  function onData(data) {
    if (busy) return;
    for (let i = 0; i < data.length; i++) {
      const ch = data[i];
      if (ch === '\t') { tab(); continue; }
      tabState = null; // any other key accepts the current text and ends the cycle
      if (ch === '\r' || ch === '\n') { if (ch === '\n' && data[i - 1] === '\r') continue; submit(); if (busy) return; continue; }
      if (ch === '\x7f' || ch === '\b') { if (cursor > 0) { line = line.slice(0, cursor - 1) + line.slice(cursor); cursor--; redraw(); } continue; }
      if (ch === '\x03') { term.write('^C'); line = ''; cursor = 0; showPrompt(); continue; }        // Ctrl-C
      if (ch === '\x15') { line = line.slice(cursor); cursor = 0; redraw(); continue; }               // Ctrl-U
      if (ch === '\x0c') { term.clear(); redraw(); continue; }                                        // Ctrl-L
      if (ch === '\x01') { cursor = 0; redraw(); continue; }                                          // Ctrl-A
      if (ch === '\x05') { cursor = line.length; redraw(); continue; }                                // Ctrl-E
      if (ch === '\x1b') { // escape sequences
        const seq = data.slice(i, i + 3);
        i += 2;
        if (seq === '\x1b[A') { if (history.length) { if (histIdx === -1) { savedLine = line; histIdx = history.length; } if (histIdx > 0) histIdx--; line = history[histIdx]; cursor = line.length; redraw(); } }
        else if (seq === '\x1b[B') { if (histIdx !== -1) { histIdx++; if (histIdx >= history.length) { histIdx = -1; line = savedLine; } else line = history[histIdx]; cursor = line.length; redraw(); } }
        else if (seq === '\x1b[C') { if (cursor < line.length) { cursor++; redraw(); } }
        else if (seq === '\x1b[D') { if (cursor > 0) { cursor--; redraw(); } }
        else if (seq === '\x1b[H') { cursor = 0; redraw(); }
        else if (seq === '\x1b[F') { cursor = line.length; redraw(); }
        else if (seq === '\x1b[3') { i++; if (cursor < line.length) { line = line.slice(0, cursor) + line.slice(cursor + 1); redraw(); } } // Delete: ESC[3~
        continue;
      }
      if (ch >= ' ') { line = line.slice(0, cursor) + ch + line.slice(cursor); cursor++; redraw(); }
    }
  }
  term.onData(onData);

  // Unsolicited output (pub/sub messages, keyspace notifications) and timers.
  const printUnsolicited = (outs) => {
    if (!outs.length) return;
    term.write('\r\x1b[2K' + outs.join('').replace(/\n/g, '\r\n'));
    redraw();
  };
  tv.exec.onUnsolicited = printUnsolicited;
  setInterval(() => printUnsolicited(tv.tick()), TICK_MS);

  term.write(promptStr());
  term.focus();
  return { term, tv };
}
