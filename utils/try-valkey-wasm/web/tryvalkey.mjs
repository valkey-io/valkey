// tryvalkey.mjs -- glue between the wasm server (tv_* exports) and cli-core.
// Works in browsers and Node; the only environment-specific thing is how the
// module factory is imported by the caller.
import { parseRESP, formatReplyTTY, CliSession } from './cli-core.mjs';

const DEFAULT_ARGS = [
  '--port', '0', '--save', '', '--appendonly', 'no', '--maxclients', '64',
  '--maxmemory', '64mb', '--maxmemory-policy', 'allkeys-lru', '--loglevel', 'warning',
  // fork()-based and process-level commands make no sense in a browser tab;
  // deny them via ACL so the visitor gets an explicit NOPERM instead of a
  // confusing fork failure.
  '--user', 'default', 'on', 'nopass', '~*', '&*', '+@all', '-bgsave', '-bgrewriteaof', '-shutdown', '-replicaof', '-debug', '-module',
];

export async function startTryValkey(createModule, { args = DEFAULT_ARGS, log = () => {}, hostLabel } = {}) {
  const Module = await createModule({ print: log, printErr: log, noInitialRun: true });
  runMain(Module, args);
  return attachTryValkey(Module, { hostLabel });
}

/* Run the server's main() with the given argv. It "returns" through
 * emscripten_exit_with_live_runtime inside aeMain, which surfaces as 'unwind'. */
export function runMain(Module, args = DEFAULT_ARGS) {
  try { Module.callMain(args); } catch (e) { if (e !== 'unwind' && e?.name !== 'ExitStatus') throw e; }
}

export { DEFAULT_ARGS };

/* Attach the CLI engine to a module whose main() has already run. */
export function attachTryValkey(Module, { hostLabel } = {}) {
  const CAP = 1 << 20;
  const buf = Module._malloc(CAP);
  const bufP = BigInt(buf);
  const lenOut = Module._malloc(4);
  const lenP = BigInt(lenOut);
  const heap = () => Module.HEAPU8; // re-read: memory growth replaces the view

  const id = Module._tv_connect();
  if (id < 0) throw new Error('tv_connect failed');
  const session = new CliSession(hostLabel);

  let inbox = new Uint8Array(0);
  const pendingReplies = []; // {argv, resolve}

  function pullOutput() {
    let got = 0;
    for (;;) {
      const n = Module._tv_read(id, bufP, CAP);
      if (n <= 0) break;
      const chunk = heap().slice(Number(buf), Number(buf) + n);
      const merged = new Uint8Array(inbox.length + chunk.length);
      merged.set(inbox); merged.set(chunk, inbox.length);
      inbox = merged; got += n;
      if (n < CAP) break;
    }
    return got;
  }

  /* Drain complete replies. Command replies resolve their promise; anything
   * else (pub/sub pushes, RESP3 push frames) is returned as unsolicited text. */
  function drain() {
    const unsolicited = [];
    for (;;) {
      let parsed;
      try { parsed = parseRESP(inbox); } catch (e) { unsolicited.push(`(error) ${e.message}\n`); inbox = new Uint8Array(0); break; }
      if (!parsed) break;
      const [reply, next] = parsed;
      inbox = inbox.slice(next);
      const isPush = reply.type === 'push' || (session.pubsubMode && pendingReplies.length === 0);
      if (isPush) {
        session.observe([], reply);
        unsolicited.push(formatReplyTTY(reply));
      } else if (pendingReplies.length) {
        const { argv, resolve, raw } = pendingReplies.shift();
        session.observe(argv, reply);
        resolve(raw ? reply : formatReplyTTY(reply));
      } else {
        unsolicited.push(formatReplyTTY(reply));
      }
    }
    return unsolicited;
  }

  /* One event-loop turn: run timers, flush replies, collect output. The
   * server writes at most ~64 KB to a client per pass (NET_MAX_WRITES_PER_EVENT),
   * so while a reply we are waiting for is still streaming out, keep turning. */
  function tick() {
    for (let i = 0; i < 1024; i++) {
      Module._tv_tick();
      const got = pullOutput();
      if (!pendingReplies.length || got === 0) break;
    }
    return drain();
  }

  /* Encode a command line with the server's own sdssplitargs(). Returns
   * { argv, bytes } or { error } for unbalanced quotes, or null for blank. */
  function encode(line) {
    const cstr = Module.stringToNewUTF8(line);
    const p = Module._tv_encode_command(BigInt(cstr), lenP);
    Module._free(cstr);
    if (Number(p) === 0) return { error: 'Invalid argument(s)\n' };
    const len = Module.getValue(Number(lenOut), 'i32');
    const bytes = heap().slice(Number(p), Number(p) + len);
    Module._tv_free(p);
    if (bytes.length === 0) return null;
    const [cmd] = parseRESP(bytes);
    return { argv: cmd.elements.map((e) => e.str), bytes };
  }

  /* Send one command line; resolves with the formatted reply text. */
  function exec(line) { return send(line, false); }
  /* Same, but resolves with the parsed reply tree (see cli-core parseRESP). */
  function execRaw(line) { return send(line, true); }
  function send(line, raw) {
    const enc = encode(line);
    if (enc === null) return Promise.resolve(raw ? null : '');
    if (enc.error) return Promise.resolve(raw ? { type: 'error', str: enc.error.trim() } : enc.error);
    return new Promise((resolve) => {
      pendingReplies.push({ argv: enc.argv, resolve, raw });
      heap().set(enc.bytes, Number(buf));
      Module._tv_write(id, bufP, enc.bytes.length);
      const out = tick();
      if (out.length) exec.onUnsolicited?.(out);
    });
  }

  /* argv for a (possibly partial) line via the server's own splitter; null if unbalanced quotes. */
  function splitArgs(line) { const enc = encode(line); return enc === null ? [] : enc.error ? null : enc.argv; }

  return { Module, session, exec, execRaw, tick, encode, splitArgs, connId: id };
}
